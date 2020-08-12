// Copyright (c) 2014-2017 The MarteX Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "anonsend-server.h"

#include "activemasternode.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "init.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "netmessagemaker.h"
#include "script/interpreter.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"

CAnonSendServer anonSendServer;

void CAnonSendServer::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(!fMasternodeMode) return;
    if(fLiteMode) return; // ignore all MarteX related functionality
    if(!masternodeSync.IsBlockchainSynced()) return;

    if(strCommand == NetMsgType::DSACCEPT) {

        if(pfrom->nVersion < MIN_ANONSEND_PEER_PROTO_VERSION) {
            LogPrint("anonsend", "DSACCEPT -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_ANONSEND_PEER_PROTO_VERSION)));
            PushStatus(pfrom, STATUS_REJECTED, ERR_VERSION, connman);
            return;
        }

        if(IsSessionReady()) {
            // too many users in this session already, reject new ones
            LogPrintf("DSACCEPT -- queue is already full!\n");
            PushStatus(pfrom, STATUS_ACCEPTED, ERR_QUEUE_FULL, connman);
            return;
        }

        CAnonsendAccept dsa;
        vRecv >> dsa;

        LogPrint("anonsend", "DSACCEPT -- nDenom %d (%s)  txCollateral %s", dsa.nDenom, CAnonSend::GetDenominationsToString(dsa.nDenom), dsa.txCollateral.ToString());

        if(dsa.nInputCount < 0 || dsa.nInputCount > ANONSEND_ENTRY_MAX_SIZE) return;

        masternode_info_t mnInfo;
        if(!mnodeman.GetMasternodeInfo(activeMasternode.outpoint, mnInfo)) {
            PushStatus(pfrom, STATUS_REJECTED, ERR_MN_LIST, connman);
            return;
        }

        if(vecSessionCollaterals.size() == 0 && mnInfo.nLastDsq != 0 &&
            mnInfo.nLastDsq + mnodeman.CountEnabled(MIN_ANONSEND_PEER_PROTO_VERSION)/5 > mnodeman.nDsqCount)
        {
            LogPrintf("DSACCEPT -- last dsq too recent, must wait: addr=%s\n", pfrom->addr.ToString());
            PushStatus(pfrom, STATUS_REJECTED, ERR_RECENT, connman);
            return;
        }

        PoolMessage nMessageID = MSG_NOERR;

        bool fResult = nSessionID == 0  ? CreateNewSession(dsa, nMessageID, connman)
                                        : AddUserToExistingSession(dsa, nMessageID);
        if(fResult) {
            LogPrintf("DSACCEPT -- is compatible, please submit!\n");
            PushStatus(pfrom, STATUS_ACCEPTED, nMessageID, connman);
            return;
        } else {
            LogPrintf("DSACCEPT -- not compatible with existing transactions!\n");
            PushStatus(pfrom, STATUS_REJECTED, nMessageID, connman);
            return;
        }

    } else if(strCommand == NetMsgType::DSQUEUE) {
        TRY_LOCK(cs_anonsend, lockRecv);
        if(!lockRecv) return;

        if(pfrom->nVersion < MIN_ANONSEND_PEER_PROTO_VERSION) {
            LogPrint("anonsend", "DSQUEUE -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_ANONSEND_PEER_PROTO_VERSION)));
            return;
        }

        CAnonsendQueue dsq;
        vRecv >> dsq;

        // process every dsq only once
        for (const auto& q : vecAnonsendQueue) {
            if(q == dsq) {
                // LogPrint("anonsend", "DSQUEUE -- %s seen\n", dsq.ToString());
                return;
            }
        }

        LogPrint("anonsend", "DSQUEUE -- %s new\n", dsq.ToString());

        if(dsq.IsExpired()) return;
        if(dsq.nInputCount < 0 || dsq.nInputCount > ANONSEND_ENTRY_MAX_SIZE) return;

        masternode_info_t mnInfo;
        if(!mnodeman.GetMasternodeInfo(dsq.masternodeOutpoint, mnInfo)) return;

        if(!dsq.CheckSignature(mnInfo.pubKeyMasternode)) {
            // we probably have outdated info
            mnodeman.AskForMN(pfrom, dsq.masternodeOutpoint, connman);
            return;
        }

        if(!dsq.fReady) {
            for (const auto& q : vecAnonsendQueue) {
                if(q.masternodeOutpoint == dsq.masternodeOutpoint) {
                    // no way same mn can send another "not yet ready" dsq this soon
                    LogPrint("anonsend", "DSQUEUE -- Masternode %s is sending WAY too many dsq messages\n", mnInfo.addr.ToString());
                    return;
                }
            }

            int nThreshold = mnInfo.nLastDsq + mnodeman.CountEnabled(MIN_ANONSEND_PEER_PROTO_VERSION)/5;
            LogPrint("anonsend", "DSQUEUE -- nLastDsq: %d  threshold: %d  nDsqCount: %d\n", mnInfo.nLastDsq, nThreshold, mnodeman.nDsqCount);
            //don't allow a few nodes to dominate the queuing process
            if(mnInfo.nLastDsq != 0 && nThreshold > mnodeman.nDsqCount) {
                LogPrint("anonsend", "DSQUEUE -- Masternode %s is sending too many dsq messages\n", mnInfo.addr.ToString());
                return;
            }
            mnodeman.AllowMixing(dsq.masternodeOutpoint);

            LogPrint("anonsend", "DSQUEUE -- new AnonSend queue (%s) from masternode %s\n", dsq.ToString(), mnInfo.addr.ToString());
            vecAnonsendQueue.push_back(dsq);
            dsq.Relay(connman);
        }

    } else if(strCommand == NetMsgType::DSVIN) {

        if(pfrom->nVersion < MIN_ANONSEND_PEER_PROTO_VERSION) {
            LogPrint("anonsend", "DSVIN -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_ANONSEND_PEER_PROTO_VERSION)));
            PushStatus(pfrom, STATUS_REJECTED, ERR_VERSION, connman);
            return;
        }

        //do we have enough users in the current session?
        if(!IsSessionReady()) {
            LogPrintf("DSVIN -- session not complete!\n");
            PushStatus(pfrom, STATUS_REJECTED, ERR_SESSION, connman);
            return;
        }

        CAnonSendEntry entry;
        vRecv >> entry;

        LogPrint("anonsend", "DSVIN -- txCollateral %s", entry.txCollateral->ToString());

        if(entry.vecTxDSIn.size() > ANONSEND_ENTRY_MAX_SIZE) {
            LogPrintf("DSVIN -- ERROR: too many inputs! %d/%d\n", entry.vecTxDSIn.size(), ANONSEND_ENTRY_MAX_SIZE);
            PushStatus(pfrom, STATUS_REJECTED, ERR_MAXIMUM, connman);
            return;
        }

        if(entry.vecTxOut.size() > ANONSEND_ENTRY_MAX_SIZE) {
            LogPrintf("DSVIN -- ERROR: too many outputs! %d/%d\n", entry.vecTxOut.size(), ANONSEND_ENTRY_MAX_SIZE);
            PushStatus(pfrom, STATUS_REJECTED, ERR_MAXIMUM, connman);
            return;
        }

        if(nSessionInputCount != 0 && entry.vecTxDSIn.size() != nSessionInputCount) {
            LogPrintf("DSVIN -- ERROR: incorrect number of inputs! %d/%d\n", entry.vecTxDSIn.size(), nSessionInputCount);
            PushStatus(pfrom, STATUS_REJECTED, ERR_INVALID_INPUT_COUNT, connman);
            return;
        }

        if(nSessionInputCount != 0 && entry.vecTxOut.size() != nSessionInputCount) {
            LogPrintf("DSVIN -- ERROR: incorrect number of outputs! %d/%d\n", entry.vecTxOut.size(), nSessionInputCount);
            PushStatus(pfrom, STATUS_REJECTED, ERR_INVALID_INPUT_COUNT, connman);
            return;
        }

        //do we have the same denominations as the current session?
        if(!IsOutputsCompatibleWithSessionDenom(entry.vecTxOut)) {
            LogPrintf("DSVIN -- not compatible with existing transactions!\n");
            PushStatus(pfrom, STATUS_REJECTED, ERR_EXISTING_TX, connman);
            return;
        }

        //check it like a transaction
        {
            CAmount nValueIn = 0;
            CAmount nValueOut = 0;

            CMutableTransaction tx;

            for (const auto& txout : entry.vecTxOut) {
                nValueOut += txout.nValue;
                tx.vout.push_back(txout);

                if(txout.scriptPubKey.size() != 25) {
                    LogPrintf("DSVIN -- non-standard pubkey detected! scriptPubKey=%s\n", ScriptToAsmStr(txout.scriptPubKey));
                    PushStatus(pfrom, STATUS_REJECTED, ERR_NON_STANDARD_PUBKEY, connman);
                    return;
                }
                if(!txout.scriptPubKey.IsPayToPublicKeyHash()) {
                    LogPrintf("DSVIN -- invalid script! scriptPubKey=%s\n", ScriptToAsmStr(txout.scriptPubKey));
                    PushStatus(pfrom, STATUS_REJECTED, ERR_INVALID_SCRIPT, connman);
                    return;
                }
            }

            for (const auto& txin : entry.vecTxDSIn) {
                tx.vin.push_back(txin);

                LogPrint("anonsend", "DSVIN -- txin=%s\n", txin.ToString());

                Coin coin;
                if(GetUTXOCoin(txin.prevout, coin)) {
                    nValueIn += coin.out.nValue;
                } else {
                    LogPrintf("DSVIN -- missing input! txin=%s\n", txin.ToString());
                    PushStatus(pfrom, STATUS_REJECTED, ERR_MISSING_TX, connman);
                    return;
                }
            }

            // There should be no fee in mixing tx
            CAmount nFee = nValueIn - nValueOut;
            if(nFee != 0) {
                LogPrintf("DSVIN -- there should be no fee in mixing tx! fees: %lld, tx=%s", nFee, tx.ToString());
                PushStatus(pfrom, STATUS_REJECTED, ERR_FEES, connman);
                return;
            }
        }

        PoolMessage nMessageID = MSG_NOERR;

        entry.addr = pfrom->addr;
        if(AddEntry(entry, nMessageID)) {
            PushStatus(pfrom, STATUS_ACCEPTED, nMessageID, connman);
            CheckPool(connman);
            RelayStatus(STATUS_ACCEPTED, connman);
        } else {
            PushStatus(pfrom, STATUS_REJECTED, nMessageID, connman);
            SetNull();
        }

    } else if(strCommand == NetMsgType::DSSIGNFINALTX) {

        if(pfrom->nVersion < MIN_ANONSEND_PEER_PROTO_VERSION) {
            LogPrint("anonsend", "DSSIGNFINALTX -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_ANONSEND_PEER_PROTO_VERSION)));
            return;
        }

        std::vector<CTxIn> vecTxIn;
        vRecv >> vecTxIn;

        LogPrint("anonsend", "DSSIGNFINALTX -- vecTxIn.size() %s\n", vecTxIn.size());

        int nTxInIndex = 0;
        int nTxInsCount = (int)vecTxIn.size();

        for (const auto& txin : vecTxIn) {
            nTxInIndex++;
            if(!AddScriptSig(txin)) {
                LogPrint("anonsend", "DSSIGNFINALTX -- AddScriptSig() failed at %d/%d, session: %d\n", nTxInIndex, nTxInsCount, nSessionID);
                RelayStatus(STATUS_REJECTED, connman);
                return;
            }
            LogPrint("anonsend", "DSSIGNFINALTX -- AddScriptSig() %d/%d success\n", nTxInIndex, nTxInsCount);
        }
        // all is good
        CheckPool(connman);
    }
}

void CAnonSendServer::SetNull()
{
    // MN side
    vecSessionCollaterals.clear();

    CAnonSendBase::SetNull();
}

//
// Check the mixing progress and send client updates if a Masternode
//
void CAnonSendServer::CheckPool(CConnman& connman)
{
    if (!fMasternodeMode) return;

    LogPrint("anonsend", "CAnonSendServer::CheckPool -- entries count %lu\n", GetEntriesCount());

    // If entries are full, create finalized transaction
    if (nState == POOL_STATE_ACCEPTING_ENTRIES && GetEntriesCount() >= CAnonSend::GetMaxPoolTransactions()) {
        LogPrint("anonsend", "CAnonSendServer::CheckPool -- FINALIZE TRANSACTIONS\n");
        CreateFinalTransaction(connman);
        return;
    }

    // If we have all of the signatures, try to compile the transaction
    if (nState == POOL_STATE_SIGNING && IsSignaturesComplete()) {
        LogPrint("anonsend", "CAnonSendServer::CheckPool -- SIGNING\n");
        CommitFinalTransaction(connman);
        return;
    }
}

void CAnonSendServer::CreateFinalTransaction(CConnman& connman)
{
    LogPrint("anonsend", "CAnonSendServer::CreateFinalTransaction -- FINALIZE TRANSACTIONS\n");

    CMutableTransaction txNew;

    // make our new transaction
    for(int i = 0; i < GetEntriesCount(); i++) {
        for (const auto& txout : vecEntries[i].vecTxOut)
            txNew.vout.push_back(txout);

        for (const auto& txdsin : vecEntries[i].vecTxDSIn)
            txNew.vin.push_back(txdsin);
    }

    sort(txNew.vin.begin(), txNew.vin.end(), CompareInputBIP69());
    sort(txNew.vout.begin(), txNew.vout.end(), CompareOutputBIP69());

    finalMutableTransaction = txNew;
    LogPrint("anonsend", "CAnonSendServer::CreateFinalTransaction -- finalMutableTransaction=%s", txNew.ToString());

    // request signatures from clients
    RelayFinalTransaction(finalMutableTransaction, connman);
    SetState(POOL_STATE_SIGNING);
}

void CAnonSendServer::CommitFinalTransaction(CConnman& connman)
{
    if(!fMasternodeMode) return; // check and relay final tx only on masternode

    CTransactionRef finalTransaction = MakeTransactionRef(finalMutableTransaction);
    uint256 hashTx = finalTransaction->GetHash();

    LogPrint("anonsend", "CAnonSendServer::CommitFinalTransaction -- finalTransaction=%s", finalTransaction->ToString());

    {
        // See if the transaction is valid
        TRY_LOCK(cs_main, lockMain);
        CValidationState validationState;
        mempool.PrioritiseTransaction(hashTx, hashTx.ToString(), 1000, 0.1*COIN);
        if(!lockMain || !AcceptToMemoryPool(mempool, validationState, finalTransaction, false, NULL, NULL, false, maxTxFee, true))
        {
            LogPrintf("CAnonSendServer::CommitFinalTransaction -- AcceptToMemoryPool() error: Transaction not valid\n");
            SetNull();
            // not much we can do in this case, just notify clients
            RelayCompletedTransaction(ERR_INVALID_TX, connman);
            return;
        }
    }

    LogPrintf("CAnonSendServer::CommitFinalTransaction -- CREATING DSTX\n");

    // create and sign masternode dstx transaction
    if(!CAnonSend::GetDSTX(hashTx)) {
        CAnonsendBroadcastTx dstxNew(finalTransaction, activeMasternode.outpoint, GetAdjustedTime());
        dstxNew.Sign();
        CAnonSend::AddDSTX(dstxNew);
    }

    LogPrintf("CAnonSendServer::CommitFinalTransaction -- TRANSMITTING DSTX\n");

    CInv inv(MSG_DSTX, hashTx);
    connman.RelayInv(inv);

    // Tell the clients it was successful
    RelayCompletedTransaction(MSG_SUCCESS, connman);

    // Randomly charge clients
    ChargeRandomFees(connman);

    // Reset
    LogPrint("anonsend", "CAnonSendServer::CommitFinalTransaction -- COMPLETED -- RESETTING\n");
    SetNull();
}

//
// Charge clients a fee if they're abusive
//
// Why bother? AnonSend uses collateral to ensure abuse to the process is kept to a minimum.
// The submission and signing stages are completely separate. In the cases where
// a client submits a transaction then refused to sign, there must be a cost. Otherwise they
// would be able to do this over and over again and bring the mixing to a halt.
//
// How does this work? Messages to Masternodes come in via NetMsgType::DSVIN, these require a valid collateral
// transaction for the client to be able to enter the pool. This transaction is kept by the Masternode
// until the transaction is either complete or fails.
//
void CAnonSendServer::ChargeFees(CConnman& connman)
{
    if(!fMasternodeMode) return;

    //we don't need to charge collateral for every offence.
    if(GetRandInt(100) > 33) return;

    std::vector<CTransactionRef> vecOffendersCollaterals;

    if(nState == POOL_STATE_ACCEPTING_ENTRIES) {
        for (const auto& txCollateral : vecSessionCollaterals) {
            bool fFound = false;
            for (const auto& entry : vecEntries)
                if(*entry.txCollateral == *txCollateral)
                    fFound = true;

            // This queue entry didn't send us the promised transaction
            if(!fFound) {
                LogPrintf("CAnonSendServer::ChargeFees -- found uncooperative node (didn't send transaction), found offence\n");
                vecOffendersCollaterals.push_back(txCollateral);
            }
        }
    }

    if(nState == POOL_STATE_SIGNING) {
        // who didn't sign?
        for (const auto& entry : vecEntries) {
            for (const auto& txdsin : entry.vecTxDSIn) {
                if(!txdsin.fHasSig) {
                    LogPrintf("CAnonSendServer::ChargeFees -- found uncooperative node (didn't sign), found offence\n");
                    vecOffendersCollaterals.push_back(entry.txCollateral);
                }
            }
        }
    }

    // no offences found
    if(vecOffendersCollaterals.empty()) return;

    //mostly offending? Charge sometimes
    if((int)vecOffendersCollaterals.size() >= Params().PoolMaxTransactions() - 1 && GetRandInt(100) > 33) return;

    //everyone is an offender? That's not right
    if((int)vecOffendersCollaterals.size() >= Params().PoolMaxTransactions()) return;

    //charge one of the offenders randomly
    std::random_shuffle(vecOffendersCollaterals.begin(), vecOffendersCollaterals.end());

    if(nState == POOL_STATE_ACCEPTING_ENTRIES || nState == POOL_STATE_SIGNING) {
        LogPrintf("CAnonSendServer::ChargeFees -- found uncooperative node (didn't %s transaction), charging fees: %s\n",
                (nState == POOL_STATE_SIGNING) ? "sign" : "send", vecOffendersCollaterals[0]->ToString());

        LOCK(cs_main);

        CValidationState state;
        if(!AcceptToMemoryPool(mempool, state, vecOffendersCollaterals[0], false, NULL, NULL, false, maxTxFee)) {
            // should never really happen
            LogPrintf("CAnonSendServer::ChargeFees -- ERROR: AcceptToMemoryPool failed!\n");
        } else {
            connman.RelayTransaction(*vecOffendersCollaterals[0]);
        }
    }
}

/*
    Charge the collateral randomly.
    Mixing is completely free, to pay miners we randomly pay the collateral of users.

    Collateral Fee Charges:

    Being that mixing has "no fees" we need to have some kind of cost associated
    with using it to stop abuse. Otherwise it could serve as an attack vector and
    allow endless transaction that would bloat MarteX and make it unusable. To
    stop these kinds of attacks 1 in 10 successful transactions are charged. This
    adds up to a cost of 0.001MXT per transaction on average.
*/
void CAnonSendServer::ChargeRandomFees(CConnman& connman)
{
    if(!fMasternodeMode) return;

    LOCK(cs_main);

    for (const auto& txCollateral : vecSessionCollaterals) {
        if(GetRandInt(100) > 10) return;
        LogPrintf("CAnonSendServer::ChargeRandomFees -- charging random fees, txCollateral=%s", txCollateral->ToString());

        CValidationState state;
        if(!AcceptToMemoryPool(mempool, state, txCollateral, false, NULL, NULL, false, maxTxFee)) {
            // should never really happen
            LogPrintf("CAnonSendServer::ChargeRandomFees -- ERROR: AcceptToMemoryPool failed!\n");
        } else {
            connman.RelayTransaction(*txCollateral);
        }
    }
}

//
// Check for various timeouts (queue objects, mixing, etc)
//
void CAnonSendServer::CheckTimeout(CConnman& connman)
{
    if(!fMasternodeMode) return;

    CheckQueue();

    int nTimeout = (nState == POOL_STATE_SIGNING) ? ANONSEND_SIGNING_TIMEOUT : ANONSEND_QUEUE_TIMEOUT;
    bool fTimeout = GetTime() - nTimeLastSuccessfulStep >= nTimeout;

    if(nState != POOL_STATE_IDLE && fTimeout) {
        LogPrint("anonsend", "CAnonSendServer::CheckTimeout -- %s timed out (%ds) -- resetting\n",
                (nState == POOL_STATE_SIGNING) ? "Signing" : "Session", nTimeout);
        ChargeFees(connman);
        SetNull();
    }
}

/*
    Check to see if we're ready for submissions from clients
    After receiving multiple dsa messages, the queue will switch to "accepting entries"
    which is the active state right before merging the transaction
*/
void CAnonSendServer::CheckForCompleteQueue(CConnman& connman)
{
    if(!fMasternodeMode) return;

    if(nState == POOL_STATE_QUEUE && IsSessionReady()) {
        SetState(POOL_STATE_ACCEPTING_ENTRIES);

        CAnonsendQueue dsq(nSessionDenom, nSessionInputCount, activeMasternode.outpoint, GetAdjustedTime(), true);
        LogPrint("anonsend", "CAnonSendServer::CheckForCompleteQueue -- queue is ready, signing and relaying (%s)\n", dsq.ToString());
        dsq.Sign();
        dsq.Relay(connman);
    }
}

// Check to make sure a given input matches an input in the pool and its scriptSig is valid
bool CAnonSendServer::IsInputScriptSigValid(const CTxIn& txin)
{
    CMutableTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int i = 0;
    int nTxInIndex = -1;
    CScript sigPubKey = CScript();

    for (const auto& entry : vecEntries) {

        for (const auto& txout : entry.vecTxOut)
            txNew.vout.push_back(txout);

        for (const auto& txdsin : entry.vecTxDSIn) {
            txNew.vin.push_back(txdsin);

            if(txdsin.prevout == txin.prevout) {
                nTxInIndex = i;
                sigPubKey = txdsin.prevPubKey;
            }
            i++;
        }
    }

    if(nTxInIndex >= 0) { //might have to do this one input at a time?
        txNew.vin[nTxInIndex].scriptSig = txin.scriptSig;
        LogPrint("anonsend", "CAnonSendServer::IsInputScriptSigValid -- verifying scriptSig %s\n", ScriptToAsmStr(txin.scriptSig).substr(0,24));
        if(!VerifyScript(txNew.vin[nTxInIndex].scriptSig, sigPubKey, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, MutableTransactionSignatureChecker(&txNew, nTxInIndex))) {
            LogPrint("anonsend", "CAnonSendServer::IsInputScriptSigValid -- VerifyScript() failed on input %d\n", nTxInIndex);
            return false;
        }
    } else {
        LogPrint("anonsend", "CAnonSendServer::IsInputScriptSigValid -- Failed to find matching input in pool, %s\n", txin.ToString());
        return false;
    }

    LogPrint("anonsend", "CAnonSendServer::IsInputScriptSigValid -- Successfully validated input and scriptSig\n");
    return true;
}

//
// Add a clients transaction to the pool
//
bool CAnonSendServer::AddEntry(const CAnonSendEntry& entryNew, PoolMessage& nMessageIDRet)
{
    if(!fMasternodeMode) return false;

    for (const auto& txin : entryNew.vecTxDSIn) {
        if(txin.prevout.IsNull()) {
            LogPrint("anonsend", "CAnonSendServer::AddEntry -- input not valid!\n");
            nMessageIDRet = ERR_INVALID_INPUT;
            return false;
        }
    }

    if(!CAnonSend::IsCollateralValid(*entryNew.txCollateral)) {
        LogPrint("anonsend", "CAnonSendServer::AddEntry -- collateral not valid!\n");
        nMessageIDRet = ERR_INVALID_COLLATERAL;
        return false;
    }

    if(GetEntriesCount() >= CAnonSend::GetMaxPoolTransactions()) {
        LogPrint("anonsend", "CAnonSendServer::AddEntry -- entries is full!\n");
        nMessageIDRet = ERR_ENTRIES_FULL;
        return false;
    }

    for (const auto& txin : entryNew.vecTxDSIn) {
        LogPrint("anonsend", "looking for txin -- %s\n", txin.ToString());
        for (const auto& entry : vecEntries) {
            for (const auto& txdsin : entry.vecTxDSIn) {
                if(txdsin.prevout == txin.prevout) {
                    LogPrint("anonsend", "CAnonSendServer::AddEntry -- found in txin\n");
                    nMessageIDRet = ERR_ALREADY_HAVE;
                    return false;
                }
            }
        }
    }

    vecEntries.push_back(entryNew);

    LogPrint("anonsend", "CAnonSendServer::AddEntry -- adding entry\n");
    nMessageIDRet = MSG_ENTRIES_ADDED;
    nTimeLastSuccessfulStep = GetTime();

    return true;
}

bool CAnonSendServer::AddScriptSig(const CTxIn& txinNew)
{
    LogPrint("anonsend", "CAnonSendServer::AddScriptSig -- scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0,24));

    for (const auto& entry : vecEntries) {
        for (const auto& txdsin : entry.vecTxDSIn) {
            if(txdsin.scriptSig == txinNew.scriptSig) {
                LogPrint("anonsend", "CAnonSendServer::AddScriptSig -- already exists\n");
                return false;
            }
        }
    }

    if(!IsInputScriptSigValid(txinNew)) {
        LogPrint("anonsend", "CAnonSendServer::AddScriptSig -- Invalid scriptSig\n");
        return false;
    }

    LogPrint("anonsend", "CAnonSendServer::AddScriptSig -- scriptSig=%s new\n", ScriptToAsmStr(txinNew.scriptSig).substr(0,24));

    for (auto& txin : finalMutableTransaction.vin) {
        if(txin.prevout == txinNew.prevout && txin.nSequence == txinNew.nSequence) {
            txin.scriptSig = txinNew.scriptSig;
            LogPrint("anonsend", "CAnonSendServer::AddScriptSig -- adding to finalMutableTransaction, scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0,24));
        }
    }
    for(int i = 0; i < GetEntriesCount(); i++) {
        if(vecEntries[i].AddScriptSig(txinNew)) {
            LogPrint("anonsend", "CAnonSendServer::AddScriptSig -- adding to entries, scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0,24));
            return true;
        }
    }

    LogPrintf("CAnonSendServer::AddScriptSig -- Couldn't set sig!\n" );
    return false;
}

// Check to make sure everything is signed
bool CAnonSendServer::IsSignaturesComplete()
{
    for (const auto& entry : vecEntries)
        for (const auto& txdsin : entry.vecTxDSIn)
            if(!txdsin.fHasSig) return false;

    return true;
}

bool CAnonSendServer::IsOutputsCompatibleWithSessionDenom(const std::vector<CTxOut>& vecTxOut)
{
    if(CAnonSend::GetDenominations(vecTxOut) == 0) return false;

    for (const auto& entry : vecEntries) {
        LogPrintf("CAnonSendServer::IsOutputsCompatibleWithSessionDenom -- vecTxOut denom %d, entry.vecTxOut denom %d\n",
                CAnonSend::GetDenominations(vecTxOut), CAnonSend::GetDenominations(entry.vecTxOut));
        if(CAnonSend::GetDenominations(vecTxOut) != CAnonSend::GetDenominations(entry.vecTxOut)) return false;
    }

    return true;
}

bool CAnonSendServer::IsAcceptableDSA(const CAnonsendAccept& dsa, PoolMessage& nMessageIDRet)
{
    if(!fMasternodeMode) return false;

    // is denom even smth legit?
    std::vector<int> vecBits;
    if(!CAnonSend::GetDenominationsBits(dsa.nDenom, vecBits)) {
        LogPrint("anonsend", "CAnonSendServer::%s -- denom not valid!\n", __func__);
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    // check collateral
    if(!fUnitTest && !CAnonSend::IsCollateralValid(dsa.txCollateral)) {
        LogPrint("anonsend", "CAnonSendServer::%s -- collateral not valid!\n", __func__);
        nMessageIDRet = ERR_INVALID_COLLATERAL;
        return false;
    }

    if(dsa.nInputCount < 0 || dsa.nInputCount > ANONSEND_ENTRY_MAX_SIZE) {
        LogPrint("anonsend", "CAnonSendServer::%s -- requested count is not valid!\n", __func__);
        nMessageIDRet = ERR_INVALID_INPUT_COUNT;
        return false;
    }

    return true;
}

bool CAnonSendServer::CreateNewSession(const CAnonsendAccept& dsa, PoolMessage& nMessageIDRet, CConnman& connman)
{
    if(!fMasternodeMode || nSessionID != 0) return false;

    // new session can only be started in idle mode
    if(nState != POOL_STATE_IDLE) {
        nMessageIDRet = ERR_MODE;
        LogPrintf("CAnonSendServer::CreateNewSession -- incompatible mode: nState=%d\n", nState);
        return false;
    }

    if(!IsAcceptableDSA(dsa, nMessageIDRet)) {
        return false;
    }

    // start new session
    nMessageIDRet = MSG_NOERR;
    nSessionID = GetRandInt(2412699)+1;
    nSessionDenom = dsa.nDenom;
    // nInputCount is not covered by legacy signature, require SPORK_6_NEW_SIGS to activate to use new algo
    // (to make sure nInputCount wasn't modified by some intermediary node)
    nSessionInputCount = sporkManager.IsSporkActive(SPORK_6_NEW_SIGS) ? dsa.nInputCount : 0;

    SetState(POOL_STATE_QUEUE);
    nTimeLastSuccessfulStep = GetTime();

    if(!fUnitTest) {
        //broadcast that I'm accepting entries, only if it's the first entry through
        CAnonsendQueue dsq(dsa.nDenom, dsa.nInputCount, activeMasternode.outpoint, GetAdjustedTime(), false);
        LogPrint("anonsend", "CAnonSendServer::CreateNewSession -- signing and relaying new queue: %s\n", dsq.ToString());
        dsq.Sign();
        dsq.Relay(connman);
        vecAnonsendQueue.push_back(dsq);
    }

    vecSessionCollaterals.push_back(MakeTransactionRef(dsa.txCollateral));
    LogPrintf("CAnonSendServer::CreateNewSession -- new session created, nSessionID: %d  nSessionDenom: %d (%s)  vecSessionCollaterals.size(): %d\n",
            nSessionID, nSessionDenom, CAnonSend::GetDenominationsToString(nSessionDenom), vecSessionCollaterals.size());

    return true;
}

bool CAnonSendServer::AddUserToExistingSession(const CAnonsendAccept& dsa, PoolMessage& nMessageIDRet)
{
    if(!fMasternodeMode || nSessionID == 0 || IsSessionReady()) return false;

    if(!IsAcceptableDSA(dsa, nMessageIDRet)) {
        return false;
    }

    // we only add new users to an existing session when we are in queue mode
    if(nState != POOL_STATE_QUEUE) {
        nMessageIDRet = ERR_MODE;
        LogPrintf("CAnonSendServer::AddUserToExistingSession -- incompatible mode: nState=%d\n", nState);
        return false;
    }

    if(dsa.nDenom != nSessionDenom) {
        LogPrintf("CAnonSendServer::AddUserToExistingSession -- incompatible denom %d (%s) != nSessionDenom %d (%s)\n",
                    dsa.nDenom, CAnonSend::GetDenominationsToString(dsa.nDenom), nSessionDenom, CAnonSend::GetDenominationsToString(nSessionDenom));
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    if(dsa.nInputCount != nSessionInputCount) {
        LogPrintf("CAnonSendServer::AddUserToExistingSession -- incompatible count %d != nSessionInputCount %d\n",
                    dsa.nInputCount, nSessionInputCount);
        nMessageIDRet = ERR_INVALID_INPUT_COUNT;
        return false;
    }

    // count new user as accepted to an existing session

    nMessageIDRet = MSG_NOERR;
    nTimeLastSuccessfulStep = GetTime();
    vecSessionCollaterals.push_back(MakeTransactionRef(dsa.txCollateral));

    LogPrintf("CAnonSendServer::AddUserToExistingSession -- new user accepted, nSessionID: %d  nSessionDenom: %d (%s)  nSessionInputCount: %d  vecSessionCollaterals.size(): %d\n",
            nSessionID, nSessionDenom, CAnonSend::GetDenominationsToString(nSessionDenom), nSessionInputCount, vecSessionCollaterals.size());

    return true;
}

void CAnonSendServer::RelayFinalTransaction(const CTransaction& txFinal, CConnman& connman)
{
    LogPrint("anonsend", "CAnonSendServer::%s -- nSessionID: %d  nSessionDenom: %d (%s)\n",
            __func__, nSessionID, nSessionDenom, CAnonSend::GetDenominationsToString(nSessionDenom));

    // final mixing tx with empty signatures should be relayed to mixing participants only
    for (const auto entry : vecEntries) {
        bool fOk = connman.ForNode(entry.addr, [&txFinal, &connman, this](CNode* pnode) {
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSFINALTX, nSessionID, txFinal));
            return true;
        });
        if(!fOk) {
            // no such node? maybe this client disconnected or our own connection went down
            RelayStatus(STATUS_REJECTED, connman);
            break;
        }
    }
}

void CAnonSendServer::PushStatus(CNode* pnode, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID, CConnman& connman)
{
    if(!pnode) return;
    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSSTATUSUPDATE, nSessionID, (int)nState, (int)vecEntries.size(), (int)nStatusUpdate, (int)nMessageID));
}

void CAnonSendServer::RelayStatus(PoolStatusUpdate nStatusUpdate, CConnman& connman, PoolMessage nMessageID)
{
    unsigned int nDisconnected{};
    // status updates should be relayed to mixing participants only
    for (const auto entry : vecEntries) {
        // make sure everyone is still connected
        bool fOk = connman.ForNode(entry.addr, [&nStatusUpdate, &nMessageID, &connman, this](CNode* pnode) {
            PushStatus(pnode, nStatusUpdate, nMessageID, connman);
            return true;
        });
        if(!fOk) {
            // no such node? maybe this client disconnected or our own connection went down
            ++nDisconnected;
        }
    }
    if (nDisconnected == 0) return; // all is clear

    // smth went wrong
    LogPrintf("CAnonSendServer::%s -- can't continue, %llu client(s) disconnected, nSessionID: %d  nSessionDenom: %d (%s)\n",
            __func__, nDisconnected, nSessionID, nSessionDenom, CAnonSend::GetDenominationsToString(nSessionDenom));

    // notify everyone else that this session should be terminated
    for (const auto& entry : vecEntries) {
        connman.ForNode(entry.addr, [&connman, this](CNode* pnode) {
            PushStatus(pnode, STATUS_REJECTED, MSG_NOERR, connman);
            return true;
        });
    }

    if(nDisconnected == vecEntries.size()) {
        // all clients disconnected, there is probably some issues with our own connection
        // do not charge any fees, just reset the pool
        SetNull();
    }
}

void CAnonSendServer::RelayCompletedTransaction(PoolMessage nMessageID, CConnman& connman)
{
    LogPrint("anonsend", "CAnonSendServer::%s -- nSessionID: %d  nSessionDenom: %d (%s)\n",
            __func__, nSessionID, nSessionDenom, CAnonSend::GetDenominationsToString(nSessionDenom));

    // final mixing tx with empty signatures should be relayed to mixing participants only
    for (const auto& entry : vecEntries) {
        bool fOk = connman.ForNode(entry.addr, [&nMessageID, &connman, this](CNode* pnode) {
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSCOMPLETE, nSessionID, (int)nMessageID));
            return true;
        });
        if(!fOk) {
            // no such node? maybe client disconnected or our own connection went down
            RelayStatus(STATUS_REJECTED, connman);
            break;
        }
    }
}

void CAnonSendServer::SetState(PoolState nStateNew)
{
    if(!fMasternodeMode) return;

    if(nStateNew == POOL_STATE_ERROR || nStateNew == POOL_STATE_SUCCESS) {
        LogPrint("anonsend", "CAnonSendServer::SetState -- Can't set state to ERROR or SUCCESS as a Masternode. \n");
        return;
    }

    LogPrintf("CAnonSendServer::SetState -- nState: %d, nStateNew: %d\n", nState, nStateNew);
    nState = nStateNew;
}

//TODO: Rename/move to core
void ThreadCheckAnonSendServer(CConnman& connman)
{
    if(fLiteMode) return; // disable all MarteX specific functionality
    if(!fMasternodeMode) return; // only run on masternodes

    static bool fOneThread;
    if(fOneThread) return;
    fOneThread = true;

    // Make this thread recognisable as the AnonSend thread
    RenameThread("martex-ps-server");

    unsigned int nTick = 0;

    while (true)
    {
        MilliSleep(1000);

        if(masternodeSync.IsBlockchainSynced() && !ShutdownRequested()) {
            nTick++;
            anonSendServer.CheckTimeout(connman);
            anonSendServer.CheckForCompleteQueue(connman);
        }
    }
}
