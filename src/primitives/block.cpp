// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "primitives/block.h"

#include "consensus/merkle.h"
#include "crypto/common.h"
#include "crypto/hashblock.h"
#include "hash.h"
#include "tinyformat.h"
#include "utilstrencodings.h"

uint256 CBlockHeader::GetHash() const
{
  if (nVersion > 6)
      return Hash(BEGIN(nVersion), END(nNonce));
  else
      return GetPoWHash();
}

uint256 CBlockHeader::GetPoWHash() const
{
 return Hash9(BEGIN(nVersion), END(nNonce));
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (unsigned int i = 0; i < vtx.size(); i++)
    {
        s << "  " << vtx[i]->ToString() << "\n";
    }
    return s.str();
}
