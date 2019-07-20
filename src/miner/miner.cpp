// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "init.h"
#include "main.h"
#include "net.h"
#include "wallet/wallet.h"
#include "tx/tx.h"
#include "tx/blockrewardtx.h"
#include "tx/blockpricemediantx.h"
#include "tx/multicoinblockrewardtx.h"
#include "persistence/txdb.h"
#include "persistence/contractdb.h"
#include "persistence/cachewrapper.h"

#include <algorithm>
#include <boost/circular_buffer.hpp>

extern CWallet *pWalletMain;
extern void SetMinerStatus(bool bStatus);
//////////////////////////////////////////////////////////////////////////////
//
// CoinMiner
//

uint64_t nLastBlockTx   = 0;
uint64_t nLastBlockSize = 0;

MinedBlockInfo miningBlockInfo;
boost::circular_buffer<MinedBlockInfo> minedBlocks(kMaxMinedBlocks);
CCriticalSection csMinedBlocks;

//base on the last 50 blocks
int GetElementForBurn(CBlockIndex *pIndex) {
    if (NULL == pIndex) {
        return INIT_FUEL_RATES;
    }
    int nBlock = SysCfg().GetArg("-blocksizeforburn", DEFAULT_BURN_BLOCK_SIZE);
    if (nBlock * 2 >= pIndex->nHeight - 1) {
        return INIT_FUEL_RATES;
    }

    int64_t nTotalStep(0);
    int64_t nAverateStep(0);
    CBlockIndex *pTemp = pIndex;
    for (int ii = 0; ii < nBlock; ii++) {
        nTotalStep += pTemp->nFuel / pTemp->nFuelRate * 100;
        pTemp = pTemp->pprev;
    }
    nAverateStep = nTotalStep / nBlock;
    int newFuelRate(0);
    if (nAverateStep < MAX_BLOCK_RUN_STEP * 0.75) {
        newFuelRate = pIndex->nFuelRate * 0.9;
    } else if (nAverateStep > MAX_BLOCK_RUN_STEP * 0.85) {
        newFuelRate = pIndex->nFuelRate * 1.1;
    } else {
        newFuelRate = pIndex->nFuelRate;
    }
    if (newFuelRate < MIN_FUEL_RATES)
        newFuelRate = MIN_FUEL_RATES;

    LogPrint("fuel", "preFuelRate=%d fuelRate=%d, nHeight=%d\n", pIndex->nFuelRate, newFuelRate, pIndex->nHeight);
    return newFuelRate;
}

// Sort transactions by priority and fee to decide priority orders to process transactions.
void GetPriorityTx(vector<TxPriority> &vecPriority, const int32_t nFuelRate) {
    vecPriority.reserve(mempool.memPoolTxs.size());
    static double dPriority  = 0;
    static double dFeePerKb  = 0;
    static uint32_t nTxSize  = 0;
    static CoinType coinType = CoinType::WUSD;
    static uint64_t nFees    = 0;

    int32_t nHeight           = chainActive.Height();
    uint64_t bcoinMedianPrice = pCdMan->pPpCache->GetBcoinMedianPrice(nHeight);
    uint64_t fcoinMedianPrice = pCdMan->pPpCache->GetFcoinMedianPrice(nHeight);
    auto GetCoinMedianPrice   = [&](const CoinType coinType) -> uint64_t {
        switch (coinType) {
            case CoinType::WICC: return bcoinMedianPrice;
            case CoinType::WGRT: return fcoinMedianPrice;
            case CoinType::WUSD: return 1;
            default: return 0;
        }
    };

    for (map<uint256, CTxMemPoolEntry>::iterator mi = mempool.memPoolTxs.begin(); mi != mempool.memPoolTxs.end(); ++mi) {
        CBaseTx *pBaseTx = mi->second.GetTransaction().get();
        if (!pBaseTx->IsCoinBase() && !pCdMan->pTxCache->HaveTx(pBaseTx->GetHash())) {
            nTxSize   = mi->second.GetTxSize();
            coinType  = std::get<0>(mi->second.GetFees());
            nFees     = std::get<1>(mi->second.GetFees());
            dFeePerKb = 1.0 * GetCoinMedianPrice(coinType) / kPercentBoost *
                        (nFees - pBaseTx->GetFuel(nFuelRate)) / nTxSize / 1000.0;
            dPriority = mi->second.GetPriority();
            vecPriority.push_back(TxPriority(dPriority, dFeePerKb, mi->second.GetTransaction()));
        }
    }
}

void IncrementExtraNonce(CBlock *pBlock, CBlockIndex *pIndexPrev, unsigned int &nExtraNonce) {
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pBlock->GetPrevBlockHash()) {
        nExtraNonce   = 0;
        hashPrevBlock = pBlock->GetPrevBlockHash();
    }
    ++nExtraNonce;

    pBlock->SetMerkleRootHash(pBlock->BuildMerkleTree());
}

bool GetCurrentDelegate(const int64_t currentTime, const vector<CRegID> &delegatesList, CRegID &delegate) {
    int64_t slot = currentTime / SysCfg().GetBlockInterval();
    int miner    = slot % IniCfg().GetTotalDelegateNum();
    delegate     = delegatesList[miner];
    LogPrint("DEBUG", "currentTime=%lld, slot=%d, miner=%d, regId=%s\n", currentTime, slot, miner,
             delegate.ToString());

    return true;
}

bool CreateBlockRewardTx(const int64_t currentTime, const CAccount &delegate, CAccountDBCache &accountCache,
                         CBlock *pBlock) {
    CBlock previousBlock;
    CBlockIndex *pBlockIndex = mapBlockIndex[pBlock->GetPrevBlockHash()];
    if (pBlock->GetHeight() != 1 || pBlock->GetPrevBlockHash() != SysCfg().GetGenesisBlockHash()) {
        if (!ReadBlockFromDisk(pBlockIndex, previousBlock))
            return ERRORMSG("read block info fail from disk");

        CAccount previousDelegate;
        if (!accountCache.GetAccount(previousBlock.vptx[0]->txUid, previousDelegate)) {
            return ERRORMSG("get preblock delegate account info error");
        }

        if (currentTime - previousBlock.GetBlockTime() < SysCfg().GetBlockInterval()) {
            if (previousDelegate.regId == delegate.regId)
                return ERRORMSG("one delegate can't produce more than one block at the same slot");
        }
    }

    if (pBlock->vptx[0]->nTxType == BLOCK_REWARD_TX) {
        auto pRewardTx          = (CBlockRewardTx *)pBlock->vptx[0].get();
        pRewardTx->txUid        = delegate.regId;
        pRewardTx->nValidHeight = pBlock->GetHeight();

    } else if (pBlock->vptx[0]->nTxType == UCOIN_BLOCK_REWARD_TX) {
        auto pRewardTx          = (CMultiCoinBlockRewardTx *)pBlock->vptx[0].get();
        pRewardTx->txUid        = delegate.regId;
        pRewardTx->nValidHeight = pBlock->GetHeight();
        pRewardTx->profits      = delegate.ComputeBlockInflateInterest(pBlock->GetHeight());
    }

    pBlock->SetNonce(GetRand(SysCfg().GetBlockMaxNonce()));
    pBlock->SetMerkleRootHash(pBlock->BuildMerkleTree());
    pBlock->SetTime(currentTime);

    vector<unsigned char> signature;
    if (pWalletMain->Sign(delegate.keyId, pBlock->ComputeSignatureHash(), signature, delegate.minerPubKey.IsValid())) {
        pBlock->SetSignature(signature);
        return true;
    } else {
        return ERRORMSG("Sign failed");
    }
}

void ShuffleDelegates(const int nCurHeight, vector<CRegID> &delegatesList) {
    uint32_t TotalDelegateNum = IniCfg().GetTotalDelegateNum();
    string seedSource = strprintf("%u", nCurHeight / TotalDelegateNum + (nCurHeight % TotalDelegateNum > 0 ? 1 : 0));
    CHashWriter ss(SER_GETHASH, 0);
    ss << seedSource;
    uint256 currendSeed  = ss.GetHash();
    uint64_t currendTemp = 0;
    for (uint32_t i = 0, delCount = TotalDelegateNum; i < delCount; i++) {
        for (uint32_t x = 0; x < 4 && i < delCount; i++, x++) {
            memcpy(&currendTemp, currendSeed.begin() + (x * 8), 8);
            uint32_t newIndex       = currendTemp % delCount;
            CRegID regId            = delegatesList[newIndex];
            delegatesList[newIndex] = delegatesList[i];
            delegatesList[i]        = regId;
        }
        ss << currendSeed;
        currendSeed = ss.GetHash();
    }
}

bool VerifyPosTx(const CBlock *pBlock, CCacheWrapper &cwIn, bool bNeedRunTx) {
    uint64_t maxNonce = SysCfg().GetBlockMaxNonce();

    vector<CRegID> delegatesList;
    if (!cwIn.delegateCache.GetTopDelegates(delegatesList))
        return false;

    ShuffleDelegates(pBlock->GetHeight(), delegatesList);

    CRegID regId;
    if (!GetCurrentDelegate(pBlock->GetTime(), delegatesList, regId))
        return ERRORMSG("VerifyPosTx() : failed to get current delegate");
    CAccount curDelegate;
    if (!cwIn.accountCache.GetAccount(regId, curDelegate))
        return ERRORMSG("VerifyPosTx() : failed to get current delegate's account, regId=%s", regId.ToString());
    if (pBlock->GetNonce() > maxNonce)
        return ERRORMSG("VerifyPosTx() : invalid nonce: %u", pBlock->GetNonce());

    if (pBlock->GetMerkleRootHash() != pBlock->BuildMerkleTree())
        return ERRORMSG("VerifyPosTx() : wrong merkle root hash");

    auto spCW = std::make_shared<CCacheWrapper>(cwIn);

    CBlockIndex *pBlockIndex = mapBlockIndex[pBlock->GetPrevBlockHash()];
    if (pBlock->GetHeight() != 1 || pBlock->GetPrevBlockHash() != SysCfg().GetGenesisBlockHash()) {
        CBlock previousBlock;
        if (!ReadBlockFromDisk(pBlockIndex, previousBlock))
            return ERRORMSG("VerifyPosTx() : read block info failed from disk");

        CAccount previousDelegate;
        if (!spCW->accountCache.GetAccount(previousBlock.vptx[0]->txUid, previousDelegate))
            return ERRORMSG("VerifyPosTx() : failed to get previous delegate's account, regId=%s",
                previousBlock.vptx[0]->txUid.ToString());

        if (pBlock->GetBlockTime() - previousBlock.GetBlockTime() < SysCfg().GetBlockInterval()) {
            if (previousDelegate.regId == curDelegate.regId)
                return ERRORMSG("VerifyPosTx() : one delegate can't produce more than one block at the same slot");
        }
    }

    CAccount account;
    if (spCW->accountCache.GetAccount(pBlock->vptx[0]->txUid, account)) {
        if (curDelegate.regId != account.regId) {
            return ERRORMSG("VerifyPosTx() : delegate should be(%s) vs what we got(%s)", curDelegate.regId.ToString(),
                            account.regId.ToString());
        }

        const uint256 &blockHash                    = pBlock->ComputeSignatureHash();
        const vector<unsigned char> &blockSignature = pBlock->GetSignature();

        if (blockSignature.size() == 0 || blockSignature.size() > MAX_BLOCK_SIGNATURE_SIZE) {
            return ERRORMSG("VerifyPosTx() : invalid block signature size, hash=%s", blockHash.ToString());
        }

        if (!VerifySignature(blockHash, blockSignature, account.pubKey))
            if (!VerifySignature(blockHash, blockSignature, account.minerPubKey))
                return ERRORMSG("VerifyPosTx() : verify signature error");
    } else {
        return ERRORMSG("VerifyPosTx() : failed to get account info, regId=%s", pBlock->vptx[0]->txUid.ToString());
    }

    if (pBlock->vptx[0]->nVersion != nTxVersion1)
        return ERRORMSG("VerifyPosTx() : transaction version %d vs current %d", pBlock->vptx[0]->nVersion, nTxVersion1);

    if (bNeedRunTx) {
        uint64_t nTotalFuel(0);
        uint64_t nTotalRunStep(0);
        for (unsigned int i = 1; i < pBlock->vptx.size(); i++) {
            shared_ptr<CBaseTx> pBaseTx = pBlock->vptx[i];
            if (spCW->txCache.HaveTx(pBaseTx->GetHash()))
                return ERRORMSG("VerifyPosTx() : duplicate transaction, txid=%s", pBaseTx->GetHash().GetHex());

            spCW->txUndo.Clear();  // Clear first.
            CValidationState state;
            if (!pBaseTx->ExecuteTx(pBlock->GetHeight(), i, *spCW, state)) {
                if (SysCfg().IsLogFailures()) {
                    pCdMan->pLogCache->SetExecuteFail(pBlock->GetHeight(), pBaseTx->GetHash(), state.GetRejectCode(),
                                                      state.GetRejectReason());
                }
                return ERRORMSG("VerifyPosTx() : failed to execute transaction, txid=%s", pBaseTx->GetHash().GetHex());
            }

            nTotalRunStep += pBaseTx->nRunStep;
            if (nTotalRunStep > MAX_BLOCK_RUN_STEP)
                return ERRORMSG("VerifyPosTx() : block total run steps(%lu) exceed max run step(%lu)", nTotalRunStep,
                                MAX_BLOCK_RUN_STEP);

            nTotalFuel += pBaseTx->GetFuel(pBlock->GetFuelRate());
            LogPrint("fuel", "VerifyPosTx() : total fuel:%d, tx fuel:%d runStep:%d fuelRate:%d txid:%s \n", nTotalFuel,
                     pBaseTx->GetFuel(pBlock->GetFuelRate()), pBaseTx->nRunStep, pBlock->GetFuelRate(),
                     pBaseTx->GetHash().GetHex());
        }

        if (nTotalFuel != pBlock->GetFuel())
            return ERRORMSG("VerifyPosTx() : total fuel(%lu) mismatch what(%u) in block header", nTotalFuel,
                            pBlock->GetFuel());
    }

    return true;
}

std::unique_ptr<CBlock> CreateNewBlock(CCacheWrapper &cwIn) {
    // Create new block
    std::unique_ptr<CBlock> pBlock(new CBlock());
    if (!pBlock.get())
        return nullptr;

    pBlock->vptx.push_back(std::make_shared<CBlockRewardTx>());
    if (GetFeatureForkVersion(currHeight) == MAJOR_VER_R1) { // pre-stablecoin release
        pBlock->vptx.push_back(std::make_shared<CBlockRewardTx>());

    } else {  //stablecoin release
        pBlock->vptx.push_back(std::make_shared<CMultiCoinBlockRewardTx>());
        pBlock->vptx.push_back(std::make_shared<CBlockPriceMedianTx>());
    }

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = SysCfg().GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to between 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE - 1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = SysCfg().GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = SysCfg().GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize              = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    {
        LOCK2(cs_main, mempool.cs);

        CBlockIndex *pIndexPrev = chainActive.Tip();
        uint32_t nHeight        = pIndexPrev->nHeight + 1;
        int32_t nFuelRate       = GetElementForBurn(pIndexPrev);
        uint64_t nBlockSize     = ::GetSerializeSize(*pBlock, SER_NETWORK, PROTOCOL_VERSION);
        uint64_t nBlockTx       = 0;
        uint64_t nTotalRunStep  = 0;
        int64_t nTotalFees      = 0;
        int64_t nTotalFuel      = 0;

        // Calculate && sort transactions from memory pool.
        vector<TxPriority> txPriorities;
        GetPriorityTx(txPriorities, nFuelRate);
        TxPriorityCompare comparer(false); // Priority by size first.
        make_heap(txPriorities.begin(), txPriorities.end(), comparer);
        LogPrint("MINER", "CreateNewBlock() : got %lu transaction(s) sorted by priority rules\n", txPriorities.size());

        // Collect transactions into the block.
        for (auto item : txPriorities) {
            // Take highest priority transaction off the priority queue.
            // TODO: Fees
            // double dFeePerKb        = std::get<1>(txPriorities.front());
            shared_ptr<CBaseTx> stx = std::get<2>(item);
            CBaseTx *pBaseTx        = stx.get();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(*pBaseTx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize) {
                LogPrint("MINER", "CreateNewBlock() : exceed max block size, txid: %s\n", pBaseTx->GetHash().GetHex());
                continue;
            }

            // Skip trx with MinRelayTxFee fee for this block
            // once the accumulated tx size surpasses the minimum block size:
            // TODO: Fees
            // if ((dFeePerKb < CBaseTx::nMinRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize)) {
            //     LogPrint("MINER", "CreateNewBlock() : skip free transaction, txid: %s\n", pBaseTx->GetHash().GetHex());
            //     continue;
            // }

            auto spCW = std::make_shared<CCacheWrapper>(cwIn);

            CValidationState state;
            pBaseTx->nFuelRate = nFuelRate;
            if (!pBaseTx->ExecuteTx(nHeight, nBlockTx + 1, *spCW, state)) {
                LogPrint("MINER", "CreateNewBlock() : failed to execute transaction, txid: %s\n",
                        pBaseTx->GetHash().GetHex());

                if (SysCfg().IsLogFailures())
                    pCdMan->pLogCache->SetExecuteFail(nHeight, pBaseTx->GetHash(), state.GetRejectCode(),
                                                      state.GetRejectReason());

                continue;
            }

            // Run step limits
            if (nTotalRunStep + pBaseTx->nRunStep >= MAX_BLOCK_RUN_STEP) {
                LogPrint("MINER", "CreateNewBlock() : exceed max block run steps, txid: %s\n",
                         pBaseTx->GetHash().GetHex());

                continue;
            }

            // Need to re-sync all to cache layer except for transaction cache, as it depends on
            // the global transaction cache to verify whether a transaction(txid) has been confirmed
            // already in block.
            spCW->Flush();

            // TODO: Fees
            // nTotalFees += pBaseTx->GetFees();
            nBlockSize += stx->GetSerializeSize(SER_NETWORK, PROTOCOL_VERSION);
            nTotalRunStep += pBaseTx->nRunStep;
            nTotalFuel += pBaseTx->GetFuel(nFuelRate);
            ++nBlockTx;
            pBlock->vptx.push_back(stx);

            LogPrint("fuel", "miner total fuel:%d, tx fuel:%d runStep:%d fuelRate:%d txid:%s\n", nTotalFuel,
                     pBaseTx->GetFuel(nFuelRate), pBaseTx->nRunStep, nFuelRate, pBaseTx->GetHash().GetHex());
        }

        nLastBlockTx               = nBlockTx;
        nLastBlockSize             = nBlockSize;
        miningBlockInfo.nTxCount   = nBlockTx;
        miningBlockInfo.nBlockSize = nBlockSize;
        miningBlockInfo.nTotalFees = nTotalFees;

        // TODO: Fees
        // assert(nTotalFees >= nTotalFuel);
        // TODO: CMultiCoinBlockRewardTx
        ((CBlockRewardTx *)pBlock->vptx[0].get())->rewardValue = nTotalFees - nTotalFuel;

        CBlockPriceMedianTx* pPriceMedianTx = ((CBlockRewardTx *)pBlock->vptx[1].get();
        map<CCoinPriceType, uint64_t> mapMedianPricePointsIn;
        cw.ppCache.GetBlockMedianPricePoints(nHeight, mapMedianPricePoints);
        pPriceMedianTx->SetMedianPricePoints(mapMedianPricePointsIn)

        // Fill in header
        pBlock->SetPrevBlockHash(pIndexPrev->GetBlockHash());
        pBlock->SetNonce(0);
        pBlock->SetHeight(nHeight);
        pBlock->SetFuel(nTotalFuel);
        pBlock->SetFuelRate(nFuelRate);
        UpdateTime(*pBlock, pIndexPrev);

        LogPrint("INFO", "CreateNewBlock(): total size %u\n", nBlockSize);
    }

    return pBlock;
}

std::unique_ptr<CBlock> CreateStableCoinGenesisBlock() {
    // Create new block
    std::unique_ptr<CBlock> pBlock(new CBlock());
    if (!pBlock.get())
        return nullptr;

    {
        LOCK(cs_main);

        pBlock->vptx.push_back(std::make_shared<CBlockRewardTx>());
        SysCfg().CreateFundCoinRewardTx(pBlock->vptx, SysCfg().NetworkID());

        // Fill in header
        CBlockIndex *pIndexPrev = chainActive.Tip();
        uint32_t nHeight        = pIndexPrev->nHeight + 1;
        int32_t nFuelRate       = GetElementForBurn(pIndexPrev);

        pBlock->SetPrevBlockHash(pIndexPrev->GetBlockHash());
        UpdateTime(*pBlock, pIndexPrev);
        pBlock->SetNonce(0);
        pBlock->SetHeight(nHeight);
        pBlock->SetFuel(0);
        pBlock->SetFuelRate(nFuelRate);
    }

    return pBlock;
}

bool CheckWork(CBlock *pBlock, CWallet &wallet) {
    // Print block information
    pBlock->Print(*pCdMan->pAccountCache);

    // Found a solution
    {
        LOCK(cs_main);
        if (pBlock->GetPrevBlockHash() != chainActive.Tip()->GetBlockHash())
            return ERRORMSG("CheckWork() : generated block is stale");

        // Process this block the same as if we received it from another node
        CValidationState state;
        if (!ProcessBlock(state, NULL, pBlock))
            return ERRORMSG("CheckWork() : failed to process block");
    }

    return true;
}

bool static MineBlock(CBlock *pBlock, CWallet *pWallet, CBlockIndex *pIndexPrev,
                        unsigned int nTransactionsUpdated, CCacheWrapper &cw) {
    int64_t nStart = GetTime();

    while (true) {
        boost::this_thread::interruption_point();

        // Should not mine new blocks if the miner does not connect to other nodes except running
        // in regtest network.
        if (vNodes.empty() && SysCfg().NetworkID() != REGTEST_NET)
            return false;

        if (pIndexPrev != chainActive.Tip())
            return false;

        // Take a sleep and check.
        [&]() {
            int64_t whenCanIStart = pIndexPrev->GetBlockTime() + SysCfg().GetBlockInterval();
            while (GetTime() < whenCanIStart) {
                ::MilliSleep(100);
            }
        } ();

        vector<CRegID> delegatesList;
        if (!cw.delegateCache.GetTopDelegates(delegatesList)) {
            LogPrint("MINER", "MineBlock() : failed to get top delegates\n");
            return false;
        }

        uint16_t nIndex = 0;
        for (auto &delegate : delegatesList)
            LogPrint("shuffle", "before shuffle: index=%d, regId=%s\n", nIndex++, delegate.ToString());

        ShuffleDelegates(pBlock->GetHeight(), delegatesList);

        nIndex = 0;
        for (auto &delegate : delegatesList)
            LogPrint("shuffle", "after shuffle: index=%d, regId=%s\n", nIndex++, delegate.ToString());

        int64_t currentTime = GetTime();
        CRegID regId;
        GetCurrentDelegate(currentTime, delegatesList, regId);
        CAccount minerAcct;
        if (!cw.accountCache.GetAccount(regId, minerAcct)) {
            LogPrint("MINER", "MineBlock() : failed to get miner's account: %s\n", regId.ToString());
            return false;
        }

        bool success = false;
        int64_t nLastTime;
        {
            LOCK2(cs_main, pWalletMain->cs_wallet);
            if (uint32_t(chainActive.Tip()->nHeight + 1) != pBlock->GetHeight())
                return false;

            CKey acctKey;
            if (pWalletMain->GetKey(minerAcct.keyId.ToAddress(), acctKey, true) ||
                pWalletMain->GetKey(minerAcct.keyId.ToAddress(), acctKey)) {
                nLastTime = GetTimeMillis();
                success   = CreateBlockRewardTx(currentTime, minerAcct, cw.accountCache, pBlock);
                LogPrint("MINER", "MineBlock() : %s to create block reward transaction, used %d ms, miner address %s\n",
                         success ? "succeed" : "failed", GetTimeMillis() - nLastTime, minerAcct.keyId.ToAddress());
            }
        }

        if (success) {
            SetThreadPriority(THREAD_PRIORITY_NORMAL);

            nLastTime = GetTimeMillis();
            CheckWork(pBlock, *pWallet);
            LogPrint("MINER", "MineBlock() : check work used %s ms\n", GetTimeMillis() - nLastTime);

            SetThreadPriority(THREAD_PRIORITY_LOWEST);

            miningBlockInfo.nTime         = pBlock->GetBlockTime();
            miningBlockInfo.nNonce        = pBlock->GetNonce();
            miningBlockInfo.nHeight       = pBlock->GetHeight();
            miningBlockInfo.nTotalFuels   = pBlock->GetFuel();
            miningBlockInfo.nFuelRate     = pBlock->GetFuelRate();
            miningBlockInfo.hash          = pBlock->GetHash();
            miningBlockInfo.hashPrevBlock = pBlock->GetHash();

            {
                LOCK(csMinedBlocks);
                minedBlocks.push_front(miningBlockInfo);
            }

            return true;
        }

        if (mempool.GetUpdatedTransactionNum() != nTransactionsUpdated || GetTime() - nStart > 60)
            return false;
    }

    return false;
}

void static CoinMiner(CWallet *pWallet, int targetHeight) {
    LogPrint("INFO", "CoinMiner() : started\n");

    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("Coin-miner");

    auto HaveMinerKey = [&]() {
        LOCK2(cs_main, pWalletMain->cs_wallet);

        set<CKeyID> setMineKey;
        setMineKey.clear();
        pWalletMain->GetKeys(setMineKey, true);
        return !setMineKey.empty();
    };

    if (!HaveMinerKey()) {
        LogPrint("ERROR", "CoinMiner() : terminated for lack of miner key\n");
        return;
    }

    auto GetCurrHeight = [&]() {
        LOCK(cs_main);
        return chainActive.Height();
    };

    targetHeight += GetCurrHeight();

    try {
        SetMinerStatus(true);

        while (true) {
            if (SysCfg().NetworkID() != REGTEST_NET) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                while (vNodes.empty() || (chainActive.Tip() && chainActive.Tip()->nHeight > 1 &&
                                          GetAdjustedTime() - chainActive.Tip()->nTime > 60 * 60 &&
                                          !SysCfg().GetBoolArg("-genblockforce", false))) {
                    MilliSleep(1000);
                }
            }

            //
            // Create new block
            //
            unsigned int nTransactionsUpdated = mempool.GetUpdatedTransactionNum();
            CBlockIndex *pIndexPrev           = chainActive.Tip();

            auto spCW = std::make_shared<CCacheWrapper>(pCdMan);

            miningBlockInfo.SetNull();
            int64_t nLastTime = GetTimeMillis();

            auto pBlock = (pIndexPrev->nHeight + 1 == (int32_t)SysCfg().GetStableCoinGenesisHeight())
                              ? CreateStableCoinGenesisBlock()
                              : CreateNewBlock(*spCW);
            if (!pBlock.get()) {
                throw runtime_error("CoinMiner() : failed to create new block");

            } else {
                LogPrint("MINER", "CoinMiner() : succeed to create new block, contain %s transactions, used %s ms\n",
                         pBlock->vptx.size(), GetTimeMillis() - nLastTime);
            }

            // Attention: need to reset delegate cache to compute the miner account according to received votes ranking
            // list.
            spCW->delegateCache.Clear();
            MineBlock(pBlock.get(), pWallet, pIndexPrev, nTransactionsUpdated, *spCW);

            if (SysCfg().NetworkID() != MAIN_NET && targetHeight <= GetCurrHeight())
                throw boost::thread_interrupted();
        }
    } catch (...) {
        LogPrint("INFO", "CoinMiner() : terminated\n");
        SetMinerStatus(false);
        throw;
    }
}

void GenerateCoinBlock(bool fGenerate, CWallet *pWallet, int targetHeight) {
    static boost::thread_group *minerThreads = NULL;

    if (minerThreads != NULL) {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (!fGenerate)
        return;

    // In mainnet, coin miner should generate blocks continuously regardless of target height.
    if (SysCfg().NetworkID() != MAIN_NET && targetHeight <= 0) {
        LogPrint("ERROR", "GenerateCoinBlock() : target height <=0 (%d)", targetHeight);
        return;
    }

    minerThreads = new boost::thread_group();
    minerThreads->create_thread(boost::bind(&CoinMiner, pWallet, targetHeight));
}

void MinedBlockInfo::SetNull() {
    nTime       = 0;
    nNonce      = 0;
    nHeight     = 0;
    nTotalFuels = 0;
    nFuelRate   = 0;
    nTotalFees  = 0;
    nTxCount    = 0;
    nBlockSize  = 0;
    hash.SetNull();
    hashPrevBlock.SetNull();
}

int64_t MinedBlockInfo::GetReward() { return nTotalFees - nTotalFuels; }

vector<MinedBlockInfo> GetMinedBlocks(unsigned int count) {
    std::vector<MinedBlockInfo> ret;
    LOCK(csMinedBlocks);
    count = std::min((unsigned int)minedBlocks.size(), count);
    for (unsigned int i = 0; i < count; i++) {
        ret.push_back(minedBlocks[i]);
    }

    return ret;
}
