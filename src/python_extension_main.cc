// cpsm - fuzzy path matcher
// Copyright (C) 2015 Jamie Liu
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/range/adaptor/reversed.hpp>
#include <boost/utility/string_ref.hpp>

#include "ctrlp_util.h"
#include "match.h"
#include "matcher.h"
#include "par_util.h"
#include "str_util.h"

namespace {

template <typename F>
class Deferred {
 public:
  explicit Deferred(F f) : f_(std::move(f)), enabled_(true) {}
  ~Deferred() {
    if (enabled_) f_();
  }

  void cancel() { enabled_ = false; }

 private:
  F f_;
  bool enabled_;
};

template <typename F>
Deferred<F> defer(F f) {
  return Deferred<F>(f);
}

struct PyObjectDeleter {
  void operator()(PyObject* const p) const { Py_DECREF(p); }
};

// Reference-owning, self-releasing PyObject smart pointer.
typedef std::unique_ptr<PyObject, PyObjectDeleter> PyObjectPtr;

unsigned int get_nr_threads(unsigned int const max_threads) {
  std::size_t nr_threads = cpsm::Thread::hardware_concurrency();
  if (!nr_threads) {
    nr_threads = 1;
  }
  if (max_threads && (nr_threads > max_threads)) {
    nr_threads = max_threads;
  }
  return nr_threads;
}

// kBatchSizeBytes is the minimum number of bytes worth of items to read from
// the Python API before starting matching.
//
// Some math indicates that contention on the lock that guards the Python API
// is avoided on average if
//
//   N <= 1 + (U / L)
//
// where n is the number of threads, U is the time that a thread spends doing
// work without holding the lock, and L is the time that a thread requires
// the lock for. But U/L is independent of batch size. (It is also highly
// dependent on what happens during a given match.)
//
// Hence the batch size is chosen to be large, in order to amortize differences
// in match times between items and limit ping-ponging of the lock, while still
// being small enough to hopefully fit in the L1 data cache, even with SMT and
// overheads taken into account. (Ultimately it's chosen empirically.)
static constexpr std::size_t kBatchSizeBytes = 8192;

}  // namespace

extern "C" {

static PyObject* cpsm_ctrlp_match(PyObject* self, PyObject* args,
                                  PyObject* kwargs) {
  static char const* kwlist[] = {"items", "query", "limit", "mmode", "ispath",
                                 "crfile", "highlight_mode", "match_crfile",
                                 "max_threads", "query_inverting_delimiter",
                                 "unicode", nullptr};
  // Required parameters.
  PyObject* items_obj;
  char const* query_data;
  Py_ssize_t query_size;
  // CtrlP-provided options.
  int limit_int = -1;
  char const* mmode_data = nullptr;
  Py_ssize_t mmode_size = 0;
  int is_path = 0;
  char const* cur_file_data = nullptr;
  Py_ssize_t cur_file_size = 0;
  // cpsm-specific options.
  char const* highlight_mode_data = nullptr;
  Py_ssize_t highlight_mode_size = 0;
  int match_crfile = 0;
  int max_threads_int = 0;
  char const* query_inverting_delimiter_data = nullptr;
  Py_ssize_t query_inverting_delimiter_size = 0;
  int unicode = 0;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "Os#|is#is#s#iis#i", const_cast<char**>(kwlist),
          &items_obj, &query_data, &query_size, &limit_int, &mmode_data,
          &mmode_size, &is_path, &cur_file_data, &cur_file_size,
          &highlight_mode_data, &highlight_mode_size, &match_crfile,
          &max_threads_int, &query_inverting_delimiter_data,
          &query_inverting_delimiter_size, &unicode)) {
    return nullptr;
  }

  // Each match needs to be associated with both a boost::string_ref (for
  // correct sorting) and the PyObject (so it can be returned).
  typedef std::pair<boost::string_ref, PyObjectPtr> Item;

  using namespace cpsm;

  try {
    std::string query(query_data, query_size);
    boost::string_ref query_inverting_delimiter(query_inverting_delimiter_data,
                                                query_inverting_delimiter_size);
    if (!query_inverting_delimiter.empty()) {
      if (query_inverting_delimiter.size() > 1) {
        throw Error("query inverting delimiter must be a single character");
      }
      query = str_join(boost::adaptors::reverse(
                           str_split(query, query_inverting_delimiter[0])),
                       "");
    }

    MatcherOpts mopts;
    mopts.cur_file = std::string(cur_file_data, cur_file_size);
    mopts.is_path = is_path;
    mopts.match_crfile = match_crfile;
    StringHandlerOpts sopts;
    sopts.unicode = unicode;
    Matcher const matcher(std::move(query), std::move(mopts),
                          StringHandler(sopts));
    auto const item_substr_fn =
        match_mode_item_substr_fn(boost::string_ref(mmode_data, mmode_size));
    std::size_t const limit = (limit_int >= 0) ? std::size_t(limit_int) : 0;
    unsigned int const max_threads =
        (max_threads_int >= 0) ? static_cast<unsigned int>(max_threads_int) : 0;
    unsigned int const nr_threads = get_nr_threads(max_threads);

    PyObjectPtr items_iter(PyObject_GetIter(items_obj));
    if (!items_iter) {
      return nullptr;
    }
    std::mutex items_mu;
    bool end_of_python_iter = false;
    bool have_python_ex = false;

    // Do matching in parallel.
    std::vector<std::vector<Match<Item>>> thread_matches(nr_threads);
    std::vector<Thread> threads;
    for (unsigned int i = 0; i < nr_threads; i++) {
      auto& matches = thread_matches[i];
      threads.emplace_back(
          [&matcher, item_substr_fn, limit, &items_iter, &items_mu,
           &end_of_python_iter, &have_python_ex, &matches]() {
            std::vector<Item> items;
            std::vector<PyObjectPtr> unmatched_objs;
            // Ensure that unmatched PyObjects are released with items_mu held,
            // even if an exception is thrown.
            auto release_unmatched_objs =
                defer([&items, &unmatched_objs, &items_mu]() {
                  std::lock_guard<std::mutex> lock(items_mu);
                  items.clear();
                  unmatched_objs.clear();
                });

            // If a limit exists, each thread should only keep that many
            // matches.
            if (limit) {
              matches.reserve(limit + 1);
            }

            std::vector<char32_t> buf, buf2;
            while (true) {
              {
                // Collect a batch (with items_mu held to guard access to the
                // Python API).
                std::lock_guard<std::mutex> lock(items_mu);
                // Drop references on unmatched PyObjects.
                unmatched_objs.clear();
                if (end_of_python_iter || have_python_ex) {
                  return;
                }
                std::size_t batch_size_bytes = 0;
                while (batch_size_bytes < kBatchSizeBytes) {
                  PyObjectPtr item_obj(PyIter_Next(items_iter.get()));
                  if (!item_obj) {
                    end_of_python_iter = true;
                    break;
                  }
                  char* item_data;
                  Py_ssize_t item_size;
                  if (PyString_AsStringAndSize(item_obj.get(), &item_data,
                                               &item_size) < 0) {
                    have_python_ex = true;
                    return;
                  }
                  items.emplace_back(boost::string_ref(item_data, item_size),
                                     std::move(item_obj));
                  batch_size_bytes += item_size;
                }
              }
              if (items.empty()) {
                return;
              }
              unmatched_objs.reserve(items.size());
              for (auto& item : items) {
                boost::string_ref item_str(item.first);
                if (item_substr_fn) {
                  item_str = item_substr_fn(item_str);
                }
                Match<Item> m(std::move(item));
                if (matcher.match(item_str, m, nullptr, &buf, &buf2)) {
                  matches.emplace_back(std::move(m));
                  if (limit) {
                    std::push_heap(matches.begin(), matches.end());
                    if (matches.size() > limit) {
                      std::pop_heap(matches.begin(), matches.end());
                      unmatched_objs.emplace_back(
                          std::move(matches.back().item.second));
                      matches.pop_back();
                    }
                  }
                } else {
                  unmatched_objs.emplace_back(std::move(m.item.second));
                }
              }
              items.clear();
            }
          });
    }
    std::size_t nr_matches = 0;
    for (unsigned int i = 0; i < nr_threads; i++) {
      threads[i].join();
      if (threads[i].has_exception()) {
        throw Error(threads[i].exception_msg());
      }
      nr_matches += thread_matches[i].size();
    }
    if (have_python_ex) {
      return nullptr;
    }

    // Combine per-thread match lists.
    std::vector<Match<Item>> all_matches;
    all_matches.reserve(nr_matches);
    for (unsigned int i = 0; i < nr_threads; i++) {
      auto& matches = thread_matches[i];
      std::move(matches.begin(), matches.end(),
                std::back_inserter(all_matches));
      matches.shrink_to_fit();
    }
    sort_limit(all_matches, limit);

    // Produce highlighting regexes.
    boost::string_ref const highlight_mode(highlight_mode_data,
                                           highlight_mode_size);
    std::vector<std::string> match_regexes;
    if (!highlight_mode.empty() && highlight_mode != "none") {
      // Rerun matching on matched items in order to obtain match positions.
      for (auto& m : all_matches) {
        std::set<CharCount> match_positions;
        boost::string_ref item_str(m.item.first);
        if (item_substr_fn) {
          item_str = item_substr_fn(item_str);
        }
        if (!matcher.match(item_str, m, &match_positions)) {
          throw Error("failed to re-match known match '", item_str,
                      "' during highlight pass");
        }
        // Adjust match positions to account for substringing.
        if (item_substr_fn) {
          std::size_t const base = item_str.data() - m.item.first.data();
          std::set<CharCount> new_match_positions;
          for (auto const pos : match_positions) {
            new_match_positions.insert(base + pos);
          }
          match_positions = new_match_positions;
        }
        get_highlight_regexes(highlight_mode, m.item.first, match_positions,
                              match_regexes);
      }
    }

    // Translate matches back to Python.
    PyObjectPtr output_tuple(PyTuple_New(2));
    if (!output_tuple) {
      return nullptr;
    }
    PyObjectPtr matches_list(PyList_New(0));
    if (!matches_list) {
      return nullptr;
    }
    for (auto const& match : all_matches) {
      if (PyList_Append(matches_list.get(), match.item.second.get()) < 0) {
        return nullptr;
      }
    }
    if (PyTuple_SetItem(output_tuple.get(), 0, matches_list.release())) {
      return nullptr;
    }
    PyObjectPtr regexes_list(PyList_New(0));
    if (!regexes_list) {
      return nullptr;
    }
    for (auto const& regex : match_regexes) {
      PyObjectPtr regex_str(
          PyString_FromStringAndSize(regex.data(), regex.size()));
      if (!regex_str) {
        return nullptr;
      }
      if (PyList_Append(regexes_list.get(), regex_str.get()) < 0) {
        return nullptr;
      }
    }
    if (PyTuple_SetItem(output_tuple.get(), 1, regexes_list.release())) {
      return nullptr;
    }
    return output_tuple.release();
  } catch (std::exception const& ex) {
    PyErr_SetString(PyExc_RuntimeError, ex.what());
    return nullptr;
  }
}

static PyMethodDef cpsm_py_methods[] = {
    {"ctrlp_match", reinterpret_cast<PyCFunction>(cpsm_ctrlp_match),
     METH_VARARGS | METH_KEYWORDS,
     "Match strings with a CtrlP-compatible interface"},
    {nullptr, nullptr, 0, nullptr}};

PyMODINIT_FUNC initcpsm_py() { Py_InitModule("cpsm_py", cpsm_py_methods); }
}
