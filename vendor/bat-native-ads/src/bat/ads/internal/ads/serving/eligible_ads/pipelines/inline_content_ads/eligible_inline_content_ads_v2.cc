/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ads/internal/ads/serving/eligible_ads/pipelines/inline_content_ads/eligible_inline_content_ads_v2.h"

#include "absl/types/optional.h"
#include "base/bind.h"
#include "bat/ads/internal/ads/ad_events/ad_events_database_table.h"
#include "bat/ads/internal/ads/serving/choose/predict_ad.h"
#include "bat/ads/internal/ads/serving/eligible_ads/exclusion_rules/exclusion_rules_util.h"
#include "bat/ads/internal/ads/serving/eligible_ads/exclusion_rules/inline_content_ads/inline_content_ad_exclusion_rules.h"
#include "bat/ads/internal/ads/serving/serving_features.h"
#include "bat/ads/internal/ads/serving/targeting/user_model_info.h"
#include "bat/ads/internal/ads_client_helper.h"
#include "bat/ads/internal/base/logging_util.h"
#include "bat/ads/internal/creatives/inline_content_ads/creative_inline_content_ads_database_table.h"
#include "bat/ads/internal/geographic/subdivision/subdivision_targeting.h"
#include "bat/ads/internal/resources/behavioral/anti_targeting/anti_targeting_resource.h"
#include "bat/ads/public/interfaces/ads.mojom-shared.h"

namespace ads::inline_content_ads {

EligibleAdsV2::EligibleAdsV2(
    geographic::SubdivisionTargeting* subdivision_targeting,
    resource::AntiTargeting* anti_targeting_resource)
    : EligibleAdsBase(subdivision_targeting, anti_targeting_resource) {}

void EligibleAdsV2::GetForUserModel(
    const targeting::UserModelInfo& user_model,
    const std::string& dimensions,
    GetEligibleAdsCallback<CreativeInlineContentAdList> callback) {
  BLOG(1, "Get eligible inline content ads");

  database::table::AdEvents database_table;
  database_table.GetForType(
      mojom::AdType::kInlineContentAd,
      [=](const bool success, const AdEventList& ad_events) {
        if (!success) {
          BLOG(1, "Failed to get ad events");
          callback(/*had_opportunity*/ false, {});
          return;
        }

        GetBrowsingHistory(user_model, ad_events, dimensions, callback);
      });
}

///////////////////////////////////////////////////////////////////////////////

void EligibleAdsV2::GetBrowsingHistory(
    const targeting::UserModelInfo& user_model,
    const AdEventList& ad_events,
    const std::string& dimensions,
    GetEligibleAdsCallback<CreativeInlineContentAdList> callback) {
  const int max_count = features::GetBrowsingHistoryMaxCount();
  const int days_ago = features::GetBrowsingHistoryDaysAgo();
  AdsClientHelper::GetInstance()->GetBrowsingHistory(
      max_count, days_ago,
      base::BindOnce(&EligibleAdsV2::GetEligibleAds, base::Unretained(this),
                     user_model, ad_events, dimensions, callback));
}

void EligibleAdsV2::GetEligibleAds(
    const targeting::UserModelInfo& user_model,
    const AdEventList& ad_events,
    const std::string& dimensions,
    const GetEligibleAdsCallback<CreativeInlineContentAdList>& callback,
    const BrowsingHistoryList& browsing_history) {
  database::table::CreativeInlineContentAds database_table;
  database_table.GetForDimensions(
      dimensions,
      [=](const bool success, const CreativeInlineContentAdList& creative_ads) {
        if (!success) {
          BLOG(1, "Failed to get ads");
          callback(/*had_opportunity*/ false, {});
          return;
        }

        if (creative_ads.empty()) {
          BLOG(1, "No eligible ads");
          callback(/*had_opportunity*/ false, {});
          return;
        }

        const CreativeInlineContentAdList eligible_creative_ads =
            FilterCreativeAds(creative_ads, ad_events, browsing_history);
        if (eligible_creative_ads.empty()) {
          BLOG(1, "No eligible ads out of " << creative_ads.size() << " ads");
          callback(/*had_opportunity*/ true, {});
          return;
        }

        const absl::optional<CreativeInlineContentAdInfo> creative_ad =
            PredictAd(user_model, ad_events, eligible_creative_ads);
        if (!creative_ad) {
          BLOG(1, "No eligible ads out of " << creative_ads.size() << " ads");
          callback(/*had_opportunity*/ true, {});
          return;
        }

        BLOG(1, eligible_creative_ads.size() << " eligible ads out of "
                                             << creative_ads.size() << " ads");

        callback(/*had_opportunity*/ true, {*creative_ad});
      });
}

CreativeInlineContentAdList EligibleAdsV2::FilterCreativeAds(
    const CreativeInlineContentAdList& creative_ads,
    const AdEventList& ad_events,
    const BrowsingHistoryList& browsing_history) {
  if (creative_ads.empty()) {
    return {};
  }

  ExclusionRules exclusion_rules(ad_events, subdivision_targeting_,
                                 anti_targeting_resource_, browsing_history);
  return ApplyExclusionRules(creative_ads, last_served_ad_, &exclusion_rules);
}

}  // namespace ads::inline_content_ads
