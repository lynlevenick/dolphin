// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to license.txt file included.

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

class GuillotineAllocator final
{
public:
  GuillotineAllocator(u32 width, u32 height);
  GuillotineAllocator(u32 width, u32 height, u32 small_threshold, u32 large_threshold);
  GuillotineAllocator(GuillotineAllocator&&) = default;
  GuillotineAllocator(const GuillotineAllocator&) = delete;
  GuillotineAllocator& operator=(const GuillotineAllocator&) = delete;

  void Clear();
  void Reset(u32 width, u32 height, u32 small_threshold, u32 large_threshold);

  bool IsEmpty();

  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }

  std::optional<MathUtil::Rectangle<int>> Allocate(u32 requested_width, u32 requested_height);

private:
  static const u32 default_small_threshold = 32;
  static const u32 default_large_threshold = 256;

  void AddFreeRect(MathUtil::Rectangle<int>&& rect);

  std::array<std::vector<MathUtil::Rectangle<int>>, 3> m_free_rects;
  u32 m_width;
  u32 m_height;
  u32 m_small_threshold;
  u32 m_large_threshold;
};
