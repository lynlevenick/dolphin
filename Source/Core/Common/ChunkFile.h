// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

// Extremely simple serialization framework.

// (mis)-features:
// + Super fast
// + Very simple
// + Same code is used for serialization and deserializaition (in most cases)
// - Zero backwards/forwards compatibility
// - Serialization code for anything complex has to be manually written.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/Inline.h"
#include "Common/Logging/Log.h"
#include "Common/Semaphore.h"

// fixme: check if hyperthreading?
static const u32 COPY_THREADS =
    std::min(std::initializer_list<u32>({std::thread::hardware_concurrency() - 2, 1}));
static const u32 ONE_MEGABYTE = 1 * 1024 * 1024;

// XXX: Replace this with std::is_trivially_copyable<T> once we stop using volatile
// on things that are put in savestates, as volatile types are not trivially copyable.
template <typename T>
constexpr bool IsTriviallyCopyable = std::is_trivially_copyable<std::remove_volatile_t<T>>::value;

// Wrapper class
class PointerWrap
{
public:
  enum Mode
  {
    MODE_READ = 1,  // load
    MODE_WRITE,     // save
    MODE_MEASURE,   // calculate size
    MODE_VERIFY,    // compare
  };

  u8** ptr;
  Mode mode;

  inline static std::vector<std::thread> threads;
  inline static Common::Semaphore copySemaphore = Common::Semaphore(0, 8);
  inline static std::mutex copyJobMutex;
  inline static std::queue<std::tuple<void *, void *, u32>> copyJobs;
  inline static std::atomic_uint32_t copyInFlight = 0;

public:
  PointerWrap(u8** ptr_, Mode mode_) : ptr(ptr_), mode(mode_) {}
  void SetMode(Mode mode_) { mode = mode_; }
  Mode GetMode() const { return mode; }
  template <typename K, class V>
  void Do(std::map<K, V>& x)
  {
    u32 count = (u32)x.size();
    Do(count);

    switch (mode)
    {
    case MODE_READ:
      for (x.clear(); count != 0; --count)
      {
        std::pair<K, V> pair;
        Do(pair.first);
        Do(pair.second);
        x.insert(pair);
      }
      break;

    case MODE_WRITE:
    case MODE_MEASURE:
    case MODE_VERIFY:
      for (auto& elem : x)
      {
        Do(elem.first);
        Do(elem.second);
      }
      break;
    }
  }

  template <typename V>
  void Do(std::set<V>& x)
  {
    u32 count = (u32)x.size();
    Do(count);

    switch (mode)
    {
    case MODE_READ:
      for (x.clear(); count != 0; --count)
      {
        V value;
        Do(value);
        x.insert(value);
      }
      break;

    case MODE_WRITE:
    case MODE_MEASURE:
    case MODE_VERIFY:
      for (const V& val : x)
      {
        Do(val);
      }
      break;
    }
  }

  template <typename T>
  void Do(std::vector<T>& x)
  {
    DoContiguousContainer(x);
  }

  template <typename T>
  void Do(std::list<T>& x)
  {
    DoContainer(x);
  }

  template <typename T>
  void Do(std::deque<T>& x)
  {
    DoContainer(x);
  }

  template <typename T>
  void Do(std::basic_string<T>& x)
  {
    DoContiguousContainer(x);
  }

  template <typename T, typename U>
  void Do(std::pair<T, U>& x)
  {
    Do(x.first);
    Do(x.second);
  }

  template <typename T>
  void Do(std::optional<T>& x)
  {
    bool present = x.has_value();
    Do(present);

    switch (mode)
    {
    case MODE_READ:
      if (present)
      {
        x = std::make_optional<T>();
        Do(x.value());
      }
      else
      {
        x = std::nullopt;
      }
      break;

    case MODE_WRITE:
    case MODE_MEASURE:
    case MODE_VERIFY:
      if (present)
        Do(x.value());

      break;
    }
  }

  template <typename T, std::size_t N>
  void DoArray(std::array<T, N>& x)
  {
    DoArray(x.data(), static_cast<u32>(x.size()));
  }

  template <typename T, typename std::enable_if_t<IsTriviallyCopyable<T>, int> = 0>
  void DoArray(T* x, u32 count)
  {
    u32 bytes = count * sizeof(T);
    if ((mode == MODE_READ || mode == MODE_WRITE) && bytes >= ONE_MEGABYTE)
    {
      DoVoidLarge(x, bytes);
    }
    else
    {
      DoVoid(x, bytes);
    }
  }

  template <typename T, typename std::enable_if_t<!IsTriviallyCopyable<T>, int> = 0>
  void DoArray(T* x, u32 count)
  {
    for (u32 i = 0; i < count; ++i)
      Do(x[i]);
  }

  template <typename T, std::size_t N>
  void DoArray(T (&arr)[N])
  {
    DoArray(arr, static_cast<u32>(N));
  }

  // The caller is required to inspect the mode of this PointerWrap
  // and deal with the pointer returned from this function themself.
  [[nodiscard]] u8* DoExternal(u32& count)
  {
    Do(count);
    u8* current = *ptr;
    *ptr += count;
    return current;
  }

  void Do(Common::Flag& flag)
  {
    bool s = flag.IsSet();
    Do(s);
    if (mode == MODE_READ)
      flag.Set(s);
  }

  template <typename T>
  void Do(std::atomic<T>& atomic)
  {
    T temp = atomic.load();
    Do(temp);
    if (mode == MODE_READ)
      atomic.store(temp);
  }

  template <typename T>
  void Do(T& x)
  {
    static_assert(IsTriviallyCopyable<T>, "Only sane for trivially copyable types");
    // Note:
    // Usually we can just use x = **ptr, etc.  However, this doesn't work
    // for unions containing BitFields (long story, stupid language rules)
    // or arrays.  This will get optimized anyway.
    DoVoid((void*)&x, sizeof(x));
  }

  template <typename T>
  void DoPOD(T& x)
  {
    DoVoid((void*)&x, sizeof(x));
  }

  void Do(bool& x)
  {
    // bool's size can vary depending on platform, which can
    // cause breakages. This treats all bools as if they were
    // 8 bits in size.
    u8 stable = static_cast<u8>(x);

    Do(stable);

    if (mode == MODE_READ)
      x = stable != 0;
  }

  template <typename T>
  void DoPointer(T*& x, T* const base)
  {
    // pointers can be more than 2^31 apart, but you're using this function wrong if you need that
    // much range
    ptrdiff_t offset = x - base;
    Do(offset);
    if (mode == MODE_READ)
    {
      x = base + offset;
    }
  }

  void DoMarker(const std::string& prevName, u32 arbitraryNumber = 0x42)
  {
    u32 cookie = arbitraryNumber;
    Do(cookie);

    if (mode == PointerWrap::MODE_READ && cookie != arbitraryNumber)
    {
      PanicAlertFmtT(
          "Error: After \"{0}\", found {1} ({2:#x}) instead of save marker {3} ({4:#x}). Aborting "
          "savestate load...",
          prevName, cookie, cookie, arbitraryNumber, arbitraryNumber);
      mode = PointerWrap::MODE_MEASURE;
    }
  }

  template <typename T, typename Functor>
  void DoEachElement(T& container, Functor member)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    container.resize(size);

    for (auto& elem : container)
      member(*this, elem);
  }

  void Wake()
  {
    u32 inFlightNow = copyInFlight.load();
    if (inFlightNow != 0)
      PanicAlertFmt("Expected 0 jobs in flight, found {}", inFlightNow);

    if (threads.empty())
    {
      for (u32 i = 0; i < COPY_THREADS; ++i)
      {
        threads.push_back(std::thread(CopyWorker));
      }
    }
  }

  void Join()
  {
    while (copyInFlight.load() != 0)
      CopyWorkUnit();
  }

private:
  template <typename T>
  void DoContiguousContainer(T& container)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    container.resize(size);

    if (size > 0)
      DoArray(&container[0], size);
  }

  template <typename T>
  void DoContainer(T& x)
  {
    DoEachElement(x, [](PointerWrap& p, typename T::value_type& elem) { p.Do(elem); });
  }

  void DoVoidLarge(void* data, u32 size)
  {
    u8* ptrSaved = *ptr;
    *ptr += size;

    bool modeIsReadOrWrite = mode == MODE_READ || mode == MODE_WRITE;
    if (!modeIsReadOrWrite)
    {
      DEBUG_ASSERT_MSG(COMMON, modeIsReadOrWrite,
                       "Savestate failure: path must be read (%d) or write (%d), got %d.\n",
                       MODE_READ, MODE_WRITE, mode);
      return; // There's nothing left to do here anyway.
    }

    {
      std::scoped_lock<std::mutex> lock(copyJobMutex);

      u32 nextStart = 0;
      while (nextStart < size)
      {
        u32 currentStart = nextStart;
        nextStart += ONE_MEGABYTE;
        if (nextStart > size)
          nextStart = size;

        void* dataOffset = static_cast<void*>(static_cast<u8*>(data) + currentStart);
        void* ptrOffset = static_cast<void*>(ptrSaved + currentStart);

        switch(mode)
        {
        case MODE_READ:
          copyJobs.push(std::make_tuple(dataOffset, ptrOffset, nextStart - currentStart));
          break;

        case MODE_WRITE:
          copyJobs.push(std::make_tuple(ptrOffset, dataOffset, nextStart - currentStart));
          break;

        default:
          // Nothing to do here, continue will avoid breaking the threadpool.
          // This should be impossible to reach, but compilers like having a
          // default case here.
          continue;
        }

        copyInFlight.fetch_add(1);
        copySemaphore.Post();
      }
    }
  }

  DOLPHIN_FORCE_INLINE void DoVoid(void* data, u32 size)
  {
    switch (mode)
    {
    case MODE_READ:
      memcpy(data, *ptr, size);
      break;

    case MODE_WRITE:
      memcpy(*ptr, data, size);
      break;

    case MODE_MEASURE:
      break;

    case MODE_VERIFY:
      DEBUG_ASSERT_MSG(COMMON, !memcmp(data, *ptr, size),
                       "Savestate verification failure: buf %p != %p (size %u).\n", data, *ptr,
                       size);
      break;
    }

    *ptr += size;
  }

  static void CopyWorkUnit()
  {
    std::tuple<void *, void *, u32> job;

    {
      std::scoped_lock<std::mutex> lock(copyJobMutex);
      if (copyJobs.empty())
        return;

      job = std::move(copyJobs.front());
      copyJobs.pop();
    }

    std::memcpy(std::get<0>(job), std::get<1>(job), std::get<2>(job));
    copyInFlight.fetch_add(-1);
  }

  static void CopyWorker()
  {

    while(true)
    {
      copySemaphore.Wait();

      CopyWorkUnit();
    }
  }
};
