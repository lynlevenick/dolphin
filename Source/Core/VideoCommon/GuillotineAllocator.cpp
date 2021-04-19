// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to license.txt file included.

#include "VideoCommon/GuillotineAllocator.h"

#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

enum : u32
{
  SMALL_BUCKET,
  MEDIUM_BUCKET,
  LARGE_BUCKET,
  NUM_BUCKETS
};

static u32 FreeListForSize(u32 width, u32 height, u32 small_threshold, u32 large_threshold)
{
  if (width >= large_threshold || height >= large_threshold)
  {
    return LARGE_BUCKET;
  }
  else if (width >= small_threshold || height >= small_threshold)
  {
    return MEDIUM_BUCKET;
  }
  else
  {
    return SMALL_BUCKET;
  }
}

static std::pair<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
Guillotine(const MathUtil::Rectangle<int>& chosen_rect, u32 requested_width, u32 requested_height)
{
  if (requested_width == static_cast<u32>(chosen_rect.GetWidth()) &&
      requested_height == static_cast<u32>(chosen_rect.GetHeight()))
  {
    return {MathUtil::Rectangle<int>{}, MathUtil::Rectangle<int>{}};
  }

  const auto leftover_to_right =
      MathUtil::Rectangle{chosen_rect.left + static_cast<int>(requested_width), chosen_rect.top,
                          chosen_rect.right, chosen_rect.top + static_cast<int>(requested_height)};
  const auto leftover_to_bottom =
      MathUtil::Rectangle{chosen_rect.left, chosen_rect.top + static_cast<int>(requested_height),
                          chosen_rect.left + static_cast<int>(requested_width), chosen_rect.bottom};

  if (leftover_to_right.GetWidth() * leftover_to_right.GetHeight() >
      leftover_to_bottom.GetWidth() * leftover_to_bottom.GetHeight())
  {
    return {leftover_to_bottom,
            MathUtil::Rectangle<int>{leftover_to_right.left, leftover_to_right.top,
                                     leftover_to_right.right, chosen_rect.bottom}};
  }
  else
  {
    return {leftover_to_right,
            MathUtil::Rectangle<int>{leftover_to_bottom.left, leftover_to_bottom.top,
                                     chosen_rect.right, leftover_to_bottom.bottom}};
  }
}

GuillotineAllocator::GuillotineAllocator(u32 width, u32 height)
    : GuillotineAllocator(width, height, default_small_threshold, default_large_threshold)
{
}

GuillotineAllocator::GuillotineAllocator(u32 width, u32 height, u32 small_threshold,
                                         u32 large_threshold)
    : m_free_rects(), m_width(width), m_height(height), m_small_threshold(small_threshold),
      m_large_threshold(large_threshold)
{
  const u32 bucket = FreeListForSize(m_width, m_height, m_small_threshold, m_large_threshold);
  m_free_rects[bucket].emplace_back(
      MathUtil::Rectangle<int>{0, 0, static_cast<int>(m_width), static_cast<int>(m_height)});
}

void GuillotineAllocator::Clear()
{
  for (auto& free_rects : m_free_rects)
    free_rects.clear();

  const u32 bucket = FreeListForSize(m_width, m_height, m_small_threshold, m_large_threshold);
  m_free_rects[bucket].emplace_back(
      MathUtil::Rectangle<int>{0, 0, static_cast<int>(m_width), static_cast<int>(m_height)});
}

void GuillotineAllocator::Reset(u32 width, u32 height, u32 small_threshold, u32 large_threshold)
{
  m_width = width;
  m_height = height;
  m_small_threshold = small_threshold;
  m_large_threshold = large_threshold;

  Clear();
}

bool GuillotineAllocator::IsEmpty()
{
  for (auto& free_rects : m_free_rects)
  {
    for (auto& free_rect : free_rects)
      return static_cast<u32>(free_rect.GetWidth()) == m_width &&
             static_cast<u32>(free_rect.GetHeight()) == m_height;
  }

  return false;  // Should be unreachable
}

std::optional<MathUtil::Rectangle<int>> GuillotineAllocator::Allocate(u32 requested_width,
                                                                      u32 requested_height)
{
  if (requested_width == 0 || requested_height == 0)
    return std::nullopt;

  const auto ideal_bucket =
      FreeListForSize(requested_width, requested_height, m_small_threshold, m_large_threshold);
  const bool use_worst_fit = ideal_bucket != SMALL_BUCKET;

  std::optional<MathUtil::Rectangle<int>> chosen = std::nullopt;
  for (u32 bucket = ideal_bucket; bucket < NUM_BUCKETS; ++bucket)
  {
    u32 candidate_score = use_worst_fit ? 0 : UINT32_MAX;
    u32 candidate_index = UINT32_MAX;

    u32 index = 0;
    for (auto& free_rect : m_free_rects[bucket])
    {
      const s64 dx = static_cast<s64>(free_rect.GetWidth()) - requested_width;
      const s64 dy = static_cast<s64>(free_rect.GetHeight()) - requested_height;

      if (dx >= 0 && dy >= 0)
      {
        if (dx == 0 || dy == 0)
        {
          candidate_index = index;
          break;
        }

        const u32 score = static_cast<u32>(std::min(dx, dy));
        if ((use_worst_fit && score > candidate_score) ||
            (!use_worst_fit && score < candidate_score))
        {
          candidate_score = score;
          candidate_index = index;
        }
      }

      index++;
    }

    if (candidate_index != UINT32_MAX)
    {
      chosen.emplace(std::move(m_free_rects[bucket].at(candidate_index)));
      m_free_rects[bucket].erase(m_free_rects[bucket].begin() + candidate_index);
      break;
    }
  }

  if (chosen.has_value())
  {
    auto& chosen_rect = chosen.value();
    auto [split_rect, leftover_rect] = Guillotine(chosen_rect, requested_width, requested_height);

    AddFreeRect(std::move(split_rect));
    AddFreeRect(std::move(leftover_rect));

    return std::optional{MathUtil::Rectangle{chosen_rect.left, chosen_rect.top,
                                             chosen_rect.left + static_cast<int>(requested_width),
                                             chosen_rect.top + static_cast<int>(requested_height)}};
  }

  return std::nullopt;
}

void GuillotineAllocator::AddFreeRect(MathUtil::Rectangle<int>&& rect)
{
  const auto bucket =
      FreeListForSize(rect.GetWidth(), rect.GetHeight(), m_small_threshold, m_large_threshold);
  m_free_rects[bucket].emplace_back(rect);
}
