/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "perception/MerchantInventory.h"
#include "WorldPacket.h"
#include "gtest/gtest.h"
#include <limits>

namespace Alles
{
namespace
{
WorldPacket Menu(uint8_t count = 1)
{
    WorldPacket packet(SMSG_LIST_INVENTORY, 9 + 32 * count);
    packet << uint64_t(42) << count;
    for (uint8_t index = 0; index < count; ++index)
        packet << uint32_t(index + 1) << uint32_t(100) << uint32_t(200) << uint32_t(10)
            << uint32_t(9) << uint32_t(0) << uint32_t(5) << uint32_t(0);
    if (!count)
        packet << uint8_t(0);
    return packet;
}
}

TEST(AllesMerchantInventory, ReadsOnlyTheDeliveredMenuLayoutAndRejectsTruncationDuplicatesAndExtraBytes)
{
    auto packet = Menu();
    auto decoded = DecodeMerchantInventory(packet);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->vendor, 42u);
    ASSERT_EQ(decoded->offers.size(), 1u);
    EXPECT_EQ(decoded->offers[0].item, 100u);
    EXPECT_EQ(decoded->offers[0].price, 9u);
    EXPECT_EQ(decoded->offers[0].bundle, 5u);
    for (std::size_t length = 0; length < packet.size(); ++length)
    {
        WorldPacket truncated(SMSG_LIST_INVENTORY);
        if (length)
            truncated.append(packet.contents(), length);
        EXPECT_FALSE(DecodeMerchantInventory(truncated)) << length;
    }
    packet << uint8_t(0);
    EXPECT_FALSE(DecodeMerchantInventory(packet));
    packet = Menu(2);
    packet.put<uint32_t>(41, 1);
    EXPECT_FALSE(DecodeMerchantInventory(packet));
    packet = Menu(0);
    ASSERT_TRUE(DecodeMerchantInventory(packet));
    EXPECT_TRUE(DecodeMerchantInventory(packet)->offers.empty());
    packet.SetOpcode(SMSG_EMOTE);
    EXPECT_FALSE(DecodeMerchantInventory(packet));
}

TEST(AllesMerchantInventory, PurchaseRespectsBundlesStockTurnInReservesAndDiscountRounding)
{
    MerchantOffer offer{1, 100, 10, 9, 5, 0};
    auto purchase = AffordablePurchase(offer, 6, 30, 10);
    ASSERT_TRUE(purchase);
    EXPECT_EQ(purchase->bundles, 2u);
    EXPECT_EQ(purchase->items, 10u);
    EXPECT_EQ(purchase->maximumPrice, 19u);
    EXPECT_FALSE(AffordablePurchase(offer, 6, 28, 10));
    EXPECT_FALSE(AffordablePurchase(offer, 11, 100, 0));
    EXPECT_FALSE(AffordablePurchase(offer, 0, 100, 0));
    offer.extendedCost = 1;
    EXPECT_FALSE(AffordablePurchase(offer, 1, 100, 0));
    offer.extendedCost = 0;
    offer.stock = std::numeric_limits<uint32_t>::max();
    EXPECT_TRUE(AffordablePurchase(offer, 11, 100, 0));
    EXPECT_FALSE(AffordablePurchase(offer, 1280, 100000, 0)); // The buy opcode's bundle count is uint8.
    offer.bundle = 0;
    EXPECT_FALSE(AffordablePurchase(offer, 1, 100, 0));
}

TEST(AllesMerchantInventory, JunkSalesCoverOnlyTheDeficitAndCannotOverflowOrReachTheGoldCap)
{
    EXPECT_EQ(JunkSaleCount(20, 7, 10, 25, 100), 3u);
    EXPECT_EQ(JunkSaleCount(2, 7, 10, 25, 100), 2u);
    EXPECT_EQ(JunkSaleCount(20, 7, 25, 25, 100), 0u);
    EXPECT_EQ(JunkSaleCount(20, 7, 10, 100, 100), 12u);
    EXPECT_EQ(JunkSaleCount(20, 7, 93, 100, 100), 0u);
    EXPECT_EQ(JunkSaleCount(20, 0, 0, 100, 100), 0u);
    EXPECT_EQ(JunkSaleCount(20, 7, 101, 200, 100), 0u);
    auto const maximum = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(JunkSaleCount(maximum, maximum, 0, maximum, maximum), 0u);
    EXPECT_EQ(JunkSaleCount(maximum, 1, 0, maximum, maximum), maximum - 1);
    EXPECT_EQ(JunkSaleCount(maximum, 2, maximum - 4, maximum, maximum), 1u);
}

}
