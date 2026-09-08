/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_MERCHANT_INVENTORY_H
#define MOD_ALLES_MERCHANT_INVENTORY_H

#include <cstdint>
#include <optional>
#include <vector>

class WorldPacket;

namespace Alles
{
struct MerchantOffer
{
    uint32_t slot = 0;
    uint32_t item = 0;
    uint32_t stock = 0;
    uint32_t price = 0;
    uint32_t bundle = 0;
    uint32_t extendedCost = 0;
};

// Ephemeral delivered menu; not an explored merchant directory or permission to buy after reload.
struct MerchantInventory
{
    uint64_t vendor = 0;
    std::vector<MerchantOffer> offers;
};

struct MerchantPurchase
{
    uint32_t slot = 0;
    uint32_t item = 0;
    uint8_t bundles = 0;
    uint32_t items = 0;
    uint64_t maximumPrice = 0;
};

std::optional<MerchantInventory> DecodeMerchantInventory(WorldPacket const& packet);
std::optional<MerchantPurchase> AffordablePurchase(MerchantOffer const& offer, uint32_t missing,
    uint32_t ownMoney, uint32_t reservedMoney);
// Caller must first establish that this owned bag stack is expendable. The normal handler verifies the sale.
uint32_t JunkSaleCount(uint32_t count, uint32_t unitPrice, uint32_t ownMoney, uint32_t desiredMoney,
    uint32_t moneyLimit);
}
#endif
