/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "MerchantInventory.h"
#include "WorldPacket.h"
#include <algorithm>
#include <limits>
#include <set>

namespace Alles
{
std::optional<MerchantInventory> DecodeMerchantInventory(WorldPacket const& packet)
{
    if (packet.GetOpcode() != SMSG_LIST_INVENTORY || packet.size() < 9 || packet.size() > 9 + 255 * 32)
        return std::nullopt;
    auto const count = packet.read<uint8_t>(8);
    if (packet.size() != (count ? 9 + std::size_t(count) * 32 : 10) || (!count && packet.read<uint8_t>(9)))
        return std::nullopt;
    MerchantInventory result{packet.read<uint64_t>(0)};
    if (!result.vendor)
        return std::nullopt;
    std::set<uint32_t> slots;
    for (unsigned index = 0; index < count; ++index)
    {
        auto const offset = 9 + index * 32;
        MerchantOffer offer{packet.read<uint32_t>(offset), packet.read<uint32_t>(offset + 4),
            packet.read<uint32_t>(offset + 12), packet.read<uint32_t>(offset + 16),
            packet.read<uint32_t>(offset + 24), packet.read<uint32_t>(offset + 28)};
        if (!offer.slot || offer.slot > 255 || !offer.item || !offer.bundle || !slots.insert(offer.slot).second)
            return std::nullopt;
        result.offers.push_back(offer);
    }
    return result;
}

uint32_t JunkSaleCount(uint32_t count, uint32_t unitPrice, uint32_t ownMoney, uint32_t desiredMoney,
    uint32_t moneyLimit)
{
    if (!count || !unitPrice || ownMoney >= desiredMoney || ownMoney >= moneyLimit)
        return 0;
    auto const needed = (uint64_t(desiredMoney) - ownMoney + unitPrice - 1) / unitPrice;
    // The core refuses a sale that reaches the money cap, before it multiplies the accepted count by price.
    auto const capacity = (uint64_t(moneyLimit) - ownMoney - 1) / unitPrice;
    return uint32_t(std::min<uint64_t>(count, std::min(needed, capacity)));
}

std::optional<MerchantPurchase> AffordablePurchase(MerchantOffer const& offer, uint32_t missing,
    uint32_t ownMoney, uint32_t reservedMoney)
{
    if (!missing || !offer.slot || offer.slot > 255 || !offer.item || !offer.bundle || offer.extendedCost
        || reservedMoney > ownMoney)
        return std::nullopt;
    uint64_t const bundles = (uint64_t(missing) + offer.bundle - 1) / offer.bundle;
    uint64_t const items = bundles * offer.bundle;
    // The menu rounds one bundle's discounted price; the server rounds the total. Bound that rounding difference.
    uint64_t const maximumPrice = bundles * offer.price + bundles - 1;
    if (bundles > 255 || items > std::numeric_limits<uint32_t>::max() || maximumPrice > ownMoney - reservedMoney
        || (offer.stock != std::numeric_limits<uint32_t>::max() && items > offer.stock))
        return std::nullopt;
    return MerchantPurchase{offer.slot, offer.item, uint8_t(bundles), uint32_t(items), maximumPrice};
}
}
