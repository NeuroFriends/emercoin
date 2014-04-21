#include <vector>
using namespace std;

#include "bitcoinrpc.h"
#include "script.h"
#include "wallet.h"
#include "base58.h"
extern CWallet* pwalletMain;
extern std::map<uint256, CTransaction> mapTransactions;

#include "namecoin.h"
#include "hooks.h"

#include <boost/xpressive/xpressive_dynamic.hpp>

using namespace json_spirit;

static const bool NAME_DEBUG = false;

template<typename T> void ConvertTo(Value& value, bool fAllowNull=false);

static const int BUG_WORKAROUND_BLOCK_START = 0;   // Bug was not exploited before block 139872, so skip checking earlier blocks
static const int BUG_WORKAROUND_BLOCK = 0;         // Point of hard fork - 150000

map<vector<unsigned char>, uint256> mapMyNames;
map<vector<unsigned char>, set<uint256> > mapNamePending;

extern uint256 SignatureHash(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType);

// forward decls
extern bool DecodeNameScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc);
extern bool Solver(const CKeyStore& keystore, const CScript& scriptPubKey, uint256 hash, int nHashType, CScript& scriptSigRet, txnouttype& whichTypeRet);
extern bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey, const CTransaction& txTo, unsigned int nIn, bool fValidatePayToScriptHash, int nHashType);
extern bool IsConflictedTx(CTxDB& txdb, const CTransaction& tx, vector<unsigned char>& name);
//extern void rescanfornames();
extern std::string _(const char* psz);
extern bool ThreadSafeAskFee(int64 nFeeRequired, const std::string& strCaption);

const int NAME_COIN_GENESIS_EXTRA = 521;

class CNamecoinHooks : public CHooks
{
public:
    virtual bool IsStandard(const CScript& scriptPubKey);
    virtual void AddToWallet(CWalletTx& tx);
    virtual bool CheckTransaction(const CTransaction& tx);
    virtual bool ConnectInputs(CTxDB& txdb,
            map<uint256, CTxIndex>& mapTestPool,
            const CTransaction& tx,
            vector<CTransaction>& vTxPrev,
            vector<CTxIndex>& vTxindex,
            const CBlockIndex* pindexBlock,
            const CDiskTxPos &txPos,
            bool fBlock,
            bool fMiner);
    virtual bool DisconnectInputs(CTxDB& txdb,
            const CTransaction& tx,
            CBlockIndex* pindexBlock);
    virtual bool ConnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex);
    virtual bool DisconnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex);
    virtual bool ExtractAddress(const CScript& script, string& address);
    virtual bool GenesisBlock(CBlock& block);
    virtual bool Lockin(int nHeight, uint256 hash);
    virtual int LockinHeight();
    virtual string IrcPrefix();
    virtual void AcceptToMemoryPool(CTxDB& txdb, const CTransaction& tx);

    virtual void MessageStart(char* pchMessageStart)
    {
        // Make the message start different
        pchMessageStart[3] = 0xfe;
    }
    virtual bool IsMine(const CTransaction& tx);
    virtual bool IsMine(const CTransaction& tx, const CTxOut& txout, bool ignore_name_new = false);
    virtual int GetOurChainID()
    {
        return 0x0001;
    }

    virtual int GetAuxPowStartBlock()
    {
        if (fTestNet)
            return 0;
        return 19200;
    }

    virtual int GetFullRetargetStartBlock()
    {
        if (fTestNet)
            return 0;
        return 19200;
    }

    string GetAlertPubkey1()
    {
        return "04ba207043c1575208f08ea6ac27ed2aedd4f84e70b874db129acb08e6109a3bbb7c479ae22565973ebf0ac0391514511a22cb9345bdb772be20cfbd38be578b0c";
    }

    string GetAlertPubkey2()
    {
        return "04fc4366270096c7e40adb8c3fcfbff12335f3079e5e7905bce6b1539614ae057ee1e61a25abdae4a7a2368505db3541cd81636af3f7c7afe8591ebc85b2a1acdd";
    }
};

vector<unsigned char> vchFromValue(const Value& value) {
    string strName = value.get_str();
    unsigned char *strbeg = (unsigned char*)strName.c_str();
    return vector<unsigned char>(strbeg, strbeg + strName.size());
}

string stringFromVch(const vector<unsigned char> &vch) {
    string res;
    vector<unsigned char>::const_iterator vi = vch.begin();
    while (vi != vch.end()) {
        res += (char)(*vi);
        vi++;
    }
    return res;
}

// Increase expiration to 36000 gradually starting at block 24000.
// Use for validation purposes and pass the chain height.
int GetExpirationDepth(int nHeight) {
    if (nHeight < 24000)
        return 12000;
    if (nHeight < 48000)
        return nHeight - 12000;
    return 36000;
}

// For display purposes, pass the name height.
int GetDisplayExpirationDepth(int nHeight) {
    if (nHeight < 12000)
        return 12000;
    return 36000;
}

int64 GetNetworkFee(int nHeight)
{
    // Speed up network fee decrease 4x starting at 24000
    if (nHeight >= 24000)
        nHeight += (nHeight - 24000) * 3;
    if ((nHeight >> 13) >= 60)
        return 0;
    int64 nStart = 50 * COIN;
    if (fTestNet)
        nStart = 10 * CENT;
    int64 nRes = nStart >> (nHeight >> 13);
    nRes -= (nRes >> 14) * (nHeight % 8192);
    return nRes;
}

int GetTxPosHeight(const CNameIndex& txPos)
{
    return txPos.nHeight;
}

int GetTxPosHeight(const CDiskTxPos& txPos)
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txPos.nFile, txPos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return pindex->nHeight;
}

int GetTxPosHeight2(const CDiskTxPos& txPos, int nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    return nHeight;
}

int GetNameHeight(CTxDB& txdb, vector<unsigned char> vchName) {
    CNameDB dbName("cr", txdb);
    //vector<CDiskTxPos> vtxPos;
    vector<CNameIndex> vtxPos;
    if (dbName.ExistsName(vchName))
    {
        if (!dbName.ReadName(vchName, vtxPos))
            return error("GetNameHeight() : failed to read from name DB");
        if (vtxPos.empty())
            return -1;
        //CDiskTxPos& txPos = vtxPos.back();
        CNameIndex& txPos = vtxPos.back();
        return GetTxPosHeight(txPos);
    }
    return -1;
}

CScript RemoveNameScriptPrefix(const CScript& scriptIn)
{
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeNameScript(scriptIn, op, vvch,  pc))
        throw runtime_error("RemoveNameScriptPrefix() : could not decode name script");
    return CScript(pc, scriptIn.end());
}

bool SignNameSignature(const CTransaction& txFrom, CTransaction& txTo, unsigned int nIn, int nHashType=SIGHASH_ALL, CScript scriptPrereq=CScript())
{
    assert(nIn < txTo.vin.size());
    CTxIn& txin = txTo.vin[nIn];
    assert(txin.prevout.n < txFrom.vout.size());
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    // Leave out the signature from the hash, since a signature can't sign itself.
    // The checksig op will also drop the signatures from its hash.

    const CScript& scriptPubKey = RemoveNameScriptPrefix(txout.scriptPubKey);
    uint256 hash = SignatureHash(scriptPrereq + txout.scriptPubKey, txTo, nIn, nHashType);

    txnouttype whichType;
    if (!Solver(*pwalletMain, scriptPubKey, hash, nHashType, txin.scriptSig, whichType))
        return false;

    txin.scriptSig = scriptPrereq + txin.scriptSig;

    // Test solution
    if (scriptPrereq.empty())
        if (!VerifyScript(txin.scriptSig, txout.scriptPubKey, txTo, nIn, false, 0))
            return false;

    return true;
}

bool IsMyName(const CTransaction& tx, const CTxOut& txout)
{
    const CScript& scriptPubKey = RemoveNameScriptPrefix(txout.scriptPubKey);
    CScript scriptSig;
    txnouttype whichType;
    if (!Solver(*pwalletMain, scriptPubKey, 0, 0, scriptSig, whichType))
        return false;
    return true;
}

bool CreateTransactionWithInputTx(const vector<pair<CScript, int64> >& vecSend, CWalletTx& wtxIn, int nTxOut, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet)
{
    int64 nValue = 0;
    BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend)
    {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
        return false;

    wtxNew.BindWallet(pwalletMain);

    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        // txdb must be opened before the mapWallet lock
        CTxDB txdb("r");
        {
            nFeeRet = nTransactionFee;
            loop
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64 nTotalValue = nValue + nFeeRet;
                printf("CreateTransactionWithInputTx: total value = %d\n", nTotalValue);
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                int64 nWtxinCredit = wtxIn.vout[nTxOut].nValue;

                // Choose coins to use
                set<pair<const CWalletTx*, unsigned int> > setCoins;
                int64 nValueIn = 0;
                printf("CreateTransactionWithInputTx: SelectCoins(%s), nTotalValue = %s, nWtxinCredit = %s\n", FormatMoney(nTotalValue - nWtxinCredit).c_str(), FormatMoney(nTotalValue).c_str(), FormatMoney(nWtxinCredit).c_str());
                if (nTotalValue - nWtxinCredit > 0)
                {
                    if (!pwalletMain->SelectCoins(nTotalValue - nWtxinCredit, wtxNew.nTime, setCoins, nValueIn))
                        return false;
                }

                printf("CreateTransactionWithInputTx: selected %d tx outs, nValueIn = %s\n", setCoins.size(), FormatMoney(nValueIn).c_str());

                vector<pair<const CWalletTx*, unsigned int> >
                    vecCoins(setCoins.begin(), setCoins.end());

                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    int64 nCredit = coin.first->vout[coin.second].nValue;
                    dPriority += (double)nCredit * coin.first->GetDepthInMainChain();
                }

                // Input tx always at first position
                vecCoins.insert(vecCoins.begin(), make_pair(&wtxIn, nTxOut));

                nValueIn += nWtxinCredit;
                dPriority += (double)nWtxinCredit * wtxIn.GetDepthInMainChain();

                // Fill a vout back to self with any change
                int64 nChange = nValueIn - nTotalValue;
                if (nChange >= CENT)
                {
                    // Note: We use a new key here to keep it from being obvious which side is the change.
                    //  The drawback is that by not reusing a previous key, the change may be lost if a
                    //  backup is restored, if the backup doesn't have the new private key for the change.
                    //  If we reused the old key, it would be possible to add code to look for and
                    //  rediscover unknown transactions that were written with keys of ours to recover
                    //  post-backup change.

                    // Reserve a new key pair from key pool
                    CPubKey vchPubKey = reservekey.GetReservedKey();
                    assert(pwalletMain->HaveKey(vchPubKey.GetID()));

                    // -------------- Fill a vout to ourself, using same address type as the payment
                    // Now sending always to hash160 (GetBitcoinAddressHash160 will return hash160, even if pubkey is used)
                    CScript scriptChange;
                    //NOTE: look at bf798734db4539a39edd6badf54a1c3aecf193e5 commit in src/wallet.cpp
                    //if (vecSend[0].first.GetBitcoinAddressHash160() != 0)
                    //    scriptChange.SetBitcoinAddress(vchPubKey);
                    //else
                     //   scriptChange << vchPubKey << OP_CHECKSIG;
                    scriptChange.SetDestination(vchPubKey.GetID());

                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    if (coin.first == &wtxIn && coin.second == nTxOut)
                    {
                        if (!SignNameSignature(*coin.first, wtxNew, nIn++))
                            throw runtime_error("could not sign name coin output");
                    }
                    else
                    {
                        if (!SignSignature(*pwalletMain, *coin.first, wtxNew, nIn++))
                            return false;
                    }
                }

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK, PROTOCOL_VERSION);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                    return false;
                dPriority /= nBytes;

                // Check that enough fee is included
                int64 nPayFee = nTransactionFee * (1 + (int64)nBytes / 1000);
                bool fAllowFree = CTransaction::AllowFree(dPriority);
                int64 nMinFee = wtxNew.GetMinFee(1, fAllowFree);
                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    printf("CreateTransactionWithInputTx: re-iterating (nFreeRet = %s)\n", FormatMoney(nFeeRet).c_str());
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions(txdb);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    printf("CreateTransactionWithInputTx succeeded:\n%s", wtxNew.ToString().c_str());
    return true;
}

// nTxOut is the output from wtxIn that we should grab
// requires cs_main lock
string SendMoneyWithInputTx(CScript scriptPubKey, int64 nValue, int64 nNetFee, CWalletTx& wtxIn, CWalletTx& wtxNew, bool fAskFee)
{
    int nTxOut = IndexOfNameOutput(wtxIn);
    CReserveKey reservekey(pwalletMain);
    int64 nFeeRequired;
    vector< pair<CScript, int64> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));

    if (nNetFee)
    {
        CScript scriptFee;
        scriptFee << OP_RETURN;
        vecSend.push_back(make_pair(scriptFee, nNetFee));
    }

    if (!CreateTransactionWithInputTx(vecSend, wtxIn, nTxOut, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if (nValue + nFeeRequired > pwalletMain->GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }

#ifdef GUI
    if (fAskFee && !uiInterface.ThreadSafeAskFee(nFeeRequired))
        return "ABORTED";
#else
    if (fAskFee && !ThreadSafeAskFee(nFeeRequired, "Emercoin"))
        return "ABORTED";
#endif

    if (!pwalletMain->CommitTransaction(wtxNew, reservekey))
        return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}

bool GetValueOfTxPos(const CNameIndex& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    vchValue = txPos.vValue;
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos.txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    hash = tx.GetHash();
    return true;
}

bool GetValueOfTxPos(const CDiskTxPos& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    if (!GetValueOfNameTx(tx, vchValue))
        return error("GetValueOfTxPos() : could not decode value from tx");
    hash = tx.GetHash();
    return true;
}

bool CNameDB::ScanNames(
        const vector<unsigned char>& vchName,
        int nMax,
        vector<pair<vector<unsigned char>, CNameIndex> >& nameScan)
        //vector<pair<vector<unsigned char>, CDiskTxPos> >& nameScan)
{
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    unsigned int fFlags = DB_SET_RANGE;
    loop
    {
        // Read next record
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        if (fFlags == DB_SET_RANGE)
            ssKey << make_pair(string("namei"), vchName);
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
        fFlags = DB_NEXT;
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;

        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType == "namei")
        {
            vector<unsigned char> vchName;
            ssKey >> vchName;
            string strName = stringFromVch(vchName);
            //vector<CDiskTxPos> vtxPos;
            vector<CNameIndex> vtxPos;
            ssValue >> vtxPos;
            //CDiskTxPos txPos;
            CNameIndex txPos;
            if (!vtxPos.empty())
            {
                txPos = vtxPos.back();
            }
            nameScan.push_back(make_pair(vchName, txPos));
        }

        if (nameScan.size() >= nMax)
            break;
    }
    pcursor->close();
    return true;
}

CHooks* InitHook()
{
    return new CNamecoinHooks();
}

bool CNamecoinHooks::IsStandard(const CScript& scriptPubKey)
{
    return true;
}

bool DecodeNameScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch)
{
    CScript::const_iterator pc = script.begin();
    return DecodeNameScript(script, op, vvch, pc);
}

bool DecodeNameScript(const CScript& script, int& op, vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc)
{
    opcodetype opcode;
    if (!script.GetOp(pc, opcode))
        return false;
    if (opcode < OP_1 || opcode > OP_16)
        return false;

    op = opcode - OP_1 + 1;

    for (;;) {
        vector<unsigned char> vch;
        if (!script.GetOp(pc, opcode, vch))
            return false;
        if (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
            break;
        if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
            return false;
        vvch.push_back(vch);
    }

    // move the pc to after any DROP or NOP
    while (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
    {
        if (!script.GetOp(pc, opcode))
            break;
    }

    pc--;

    if ((op == OP_NAME_NEW && vvch.size() == 1) ||
            (op == OP_NAME_FIRSTUPDATE && vvch.size() == 3) ||
            (op == OP_NAME_UPDATE && vvch.size() == 2))
        return true;
    return error("invalid number of arguments for name op");
}

bool GetTxOfName(CNameDB& dbName, const vector<unsigned char> &vchName, CTransaction& tx)
{
    //vector<CDiskTxPos> vtxPos;
    vector<CNameIndex> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;
    //CDiskTxPos& txPos = vtxPos.back();
    CNameIndex& txPos = vtxPos.back();
    //int nHeight = GetTxPosHeight(txPos);
    int nHeight = txPos.nHeight;
    if (nHeight + GetExpirationDepth(pindexBest->nHeight) < pindexBest->nHeight)
    {
        string name = stringFromVch(vchName);
        printf("GetTxOfName(%s) : expired", name.c_str());
        return false;
    }

    if (!tx.ReadFromDisk(txPos.txPos))
        return error("GetTxOfName() : could not read tx from disk");
    return true;
}



//TODO: find out if it is possible to implement GetBitcoinAddress() when using multisignatures. Look at bf798734db4539a39edd6badf54a1c3aecf193e5
//      this is needed for RPC: calls name_show, name_history, name_list, sendtoname

//bool GetNameAddress(const CTransaction& tx, std::string& strAddress)
//{
//    int op;
//    int nOut;
//    vector<vector<unsigned char> > vvch;
//    if (!DecodeNameTx(tx, op, nOut, vvch, BUG_WORKAROUND_BLOCK))
//        return false;
//    const CTxOut& txout = tx.vout[nOut];
//    const CScript& scriptPubKey = RemoveNameScriptPrefix(txout.scriptPubKey);
//    strAddress = scriptPubKey.GetBitcoinAddress();
//    return true;
//}

//bool GetNameAddress(const CDiskTxPos& txPos, std::string& strAddress)
//{
//    CTransaction tx;
//    if (!tx.ReadFromDisk(txPos))
//        return error("GetNameAddress() : could not read tx from disk");

//    return GetNameAddress(tx, strAddress);
//}

//Value sendtoname(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size() < 2 || params.size() > 4)
//        throw runtime_error(
//            "sendtoname <namecoinname> <amount> [comment] [comment-to]\n"
//            "<amount> is a real and is rounded to the nearest 0.01"
//            + HelpRequiringPassphrase());

//    vector<unsigned char> vchName = vchFromValue(params[0]);
//    CNameDB dbName("r");
//    if (!dbName.ExistsName(vchName))
//        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Name not found");

//    string strAddress;
//    CTransaction tx;
//    GetTxOfName(dbName, vchName, tx);
//    GetNameAddress(tx, strAddress);

//    uint160 hash160;
//    if (!AddressToHash160(strAddress, hash160))
//        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No valid namecoin address");

//    // Amount
//    int64 nAmount = AmountFromValue(params[1]);

//    // Wallet comments
//    CWalletTx wtx;
//    if (params.size() > 2 && params[2].type() != null_type && !params[2].get_str().empty())
//        wtx.mapValue["comment"] = params[2].get_str();
//    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
//        wtx.mapValue["to"]      = params[3].get_str();

//    {
//        LOCK(cs_main);
//        EnsureWalletIsUnlocked();

//        string strError = pwalletMain->SendMoneyToBitcoinAddress(strAddress, nAmount, wtx);
//        if (strError != "")
//            throw JSONRPCError(RPC_WALLET_ERROR, strError);
//    }

//    return wtx.GetHash().GetHex();
//}

//Value name_list(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size() > 1)
//        throw runtime_error(
//                "name_list [<name>]\n"
//                "list my own names"
//                );

//    vector<unsigned char> vchName;
//    vector<unsigned char> vchLastName;
//    int nMax = 10000000;

//    if (params.size() == 1)
//    {
//        vchName = vchFromValue(params[0]);
//        nMax = 1;
//    }

//    vector<unsigned char> vchNameUniq;
//    if (params.size() == 1)
//    {
//        vchNameUniq = vchFromValue(params[0]);
//    }

//    Array oRes;
//    map< vector<unsigned char>, int > vNamesI;
//    map< vector<unsigned char>, Object > vNamesO;

//    {
//        LOCK(pwalletMain->cs_wallet);
//        CTxIndex txindex;
//        uint256 hash;
//        CTxDB txdb("r");
//        CTransaction tx;

//        vector<unsigned char> vchName;
//        vector<unsigned char> vchValue;
//        int nHeight;

//        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
//        {
//            hash = item.second.GetHash();
//            if(!txdb.ReadDiskTx(hash, tx, txindex))
//                continue;

//            if (tx.nVersion != NAMECOIN_TX_VERSION)
//                continue;

//            // name
//            if(!GetNameOfTx(tx, vchName))
//                continue;
//            if(vchNameUniq.size() > 0 && vchNameUniq != vchName)
//                continue;

//            // value
//            if(!GetValueOfNameTx(tx, vchValue))
//                continue;

//            // height
//            nHeight = GetTxPosHeight(txindex.pos);

//            Object oName;
//            oName.push_back(Pair("name", stringFromVch(vchName)));
//            oName.push_back(Pair("value", stringFromVch(vchValue)));
//            if (!hooks->IsMine(pwalletMain->mapWallet[tx.GetHash()]))
//                oName.push_back(Pair("transferred", 1));
//            string strAddress = "";
//            GetNameAddress(tx, strAddress);
//            oName.push_back(Pair("address", strAddress));
//            oName.push_back(Pair("expires_in", nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
//            if(nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
//            {
//                oName.push_back(Pair("expired", 1));
//            }

//            // get last active name only
//            if(vNamesI.find(vchName) != vNamesI.end() && vNamesI[vchName] > nHeight)
//                continue;

//            vNamesI[vchName] = nHeight;
//            vNamesO[vchName] = oName;
//        }
//    }

//    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, Object)& item, vNamesO)
//        oRes.push_back(item.second);

//    return oRes;
//}

Value name_debug(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1)
        throw runtime_error(
            "name_debug\n"
            "Dump pending transactions id in the debug file.\n");

    printf("Pending:\n----------------------------\n");
    pair<vector<unsigned char>, set<uint256> > pairPending;

    {
        LOCK(cs_main);
        BOOST_FOREACH(pairPending, mapNamePending)
        {
            string name = stringFromVch(pairPending.first);
            printf("%s :\n", name.c_str());
            uint256 hash;
            BOOST_FOREACH(hash, pairPending.second)
            {
                printf("    ");
                if (!pwalletMain->mapWallet.count(hash))
                    printf("foreign ");
                printf("    %s\n", hash.GetHex().c_str());
            }
        }
    }
    printf("----------------------------\n");
    return true;
}

Value name_debug1(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1)
        throw runtime_error(
            "name_debug1 <name>\n"
            "Dump name blocks number and transactions id in the debug file.\n");

    vector<unsigned char> vchName = vchFromValue(params[0]);
    printf("Dump name:\n");
    {
        LOCK(cs_main);
        //vector<CDiskTxPos> vtxPos;
        vector<CNameIndex> vtxPos;
        CNameDB dbName("r");
        if (!dbName.ReadName(vchName, vtxPos))
        {
            error("failed to read from name DB");
            return false;
        }
        //CDiskTxPos txPos;
        CNameIndex txPos;
        BOOST_FOREACH(txPos, vtxPos)
        {
            CTransaction tx;
            if (!tx.ReadFromDisk(txPos.txPos))
            {
                error("could not read txpos %s", txPos.txPos.ToString().c_str());
                continue;
            }
            printf("@%d %s\n", GetTxPosHeight(txPos), tx.GetHash().GetHex().c_str());
        }
    }
    printf("-------------------------\n");
    return true;
}

//Value name_show(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size() != 1)
//        throw runtime_error(
//            "name_show <name>\n"
//            "Show values of a name.\n"
//            );

//    Object oLastName;
//    vector<unsigned char> vchName = vchFromValue(params[0]);
//    string name = stringFromVch(vchName);
//    {
//        LOCK(cs_main);
//        //vector<CDiskTxPos> vtxPos;
//        vector<CNameIndex> vtxPos;
//        CNameDB dbName("r");
//        if (!dbName.ReadName(vchName, vtxPos))
//            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from name DB");

//        if (vtxPos.size() < 1)
//            throw JSONRPCError(RPC_WALLET_ERROR, "no result returned");

//        CDiskTxPos txPos = vtxPos[vtxPos.size() - 1].txPos;
//        CTransaction tx;
//        if (!tx.ReadFromDisk(txPos))
//            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from from disk");

//        Object oName;
//        vector<unsigned char> vchValue;
//        int nHeight;
//        uint256 hash;
//        if (!txPos.IsNull() && GetValueOfTxPos(txPos, vchValue, hash, nHeight))
//        {
//            oName.push_back(Pair("name", name));
//            string value = stringFromVch(vchValue);
//            oName.push_back(Pair("value", value));
//            oName.push_back(Pair("txid", tx.GetHash().GetHex()));
//            string strAddress = "";
//            GetNameAddress(txPos, strAddress);
//            oName.push_back(Pair("address", strAddress));
//            oName.push_back(Pair("expires_in", nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
//            if(nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
//            {
//                oName.push_back(Pair("expired", 1));
//            }
//            oLastName = oName;
//        }
//    }
//    return oLastName;
//}

//Value name_history(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size() != 1)
//        throw runtime_error(
//            "name_history <name>\n"
//            "List all name values of a name.\n");

//    Array oRes;
//    vector<unsigned char> vchName = vchFromValue(params[0]);
//    string name = stringFromVch(vchName);
//    {
//        LOCK(cs_main);
//        //vector<CDiskTxPos> vtxPos;
//        vector<CNameIndex> vtxPos;
//        CNameDB dbName("r");
//        if (!dbName.ReadName(vchName, vtxPos))
//            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from name DB");

//        CNameIndex txPos2;
//        CDiskTxPos txPos;
//        BOOST_FOREACH(txPos2, vtxPos)
//        {
//            txPos = txPos2.txPos;
//            CTransaction tx;
//            if (!tx.ReadFromDisk(txPos))
//            {
//                error("could not read txpos %s", txPos.ToString().c_str());
//                continue;
//            }

//            Object oName;
//            vector<unsigned char> vchValue;
//            int nHeight;
//            uint256 hash;
//            if (!txPos.IsNull() && GetValueOfTxPos(txPos, vchValue, hash, nHeight))
//            {
//                oName.push_back(Pair("name", name));
//                string value = stringFromVch(vchValue);
//                oName.push_back(Pair("value", value));
//                oName.push_back(Pair("txid", tx.GetHash().GetHex()));
//                string strAddress = "";
//                GetNameAddress(txPos, strAddress);
//                oName.push_back(Pair("address", strAddress));
//                oName.push_back(Pair("expires_in", nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
//                if(nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
//                {
//                    oName.push_back(Pair("expired", 1));
//                }
//                oRes.push_back(oName);
//            }
//        }
//    }
//    return oRes;
//}

Value name_filter(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 5)
        throw runtime_error(
                "name_filter [[[[[regexp] maxage=36000] from=0] nb=0] stat]\n"
                "scan and filter names\n"
                "[regexp] : apply [regexp] on names, empty means all names\n"
                "[maxage] : look in last [maxage] blocks\n"
                "[from] : show results from number [from]\n"
                "[nb] : show [nb] results, 0 means all\n"
                "[stats] : show some stats instead of results\n"
                "name_filter \"\" 5 # list names updated in last 5 blocks\n"
                "name_filter \"^id/\" # list all names from the \"id\" namespace\n"
                "name_filter \"^id/\" 36000 0 0 stat # display stats (number of names) on active names from the \"id\" namespace\n"
                );

    string strRegexp;
    int nFrom = 0;
    int nNb = 0;
    int nMaxAge = 36000;
    bool fStat = false;
    int nCountFrom = 0;
    int nCountNb = 0;


    if (params.size() > 0)
        strRegexp = params[0].get_str();

    if (params.size() > 1)
        nMaxAge = params[1].get_int();

    if (params.size() > 2)
        nFrom = params[2].get_int();

    if (params.size() > 3)
        nNb = params[3].get_int();

    if (params.size() > 4)
        fStat = (params[4].get_str() == "stat" ? true : false);


    CNameDB dbName("r");
    Array oRes;

    vector<unsigned char> vchName;
    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!dbName.ScanNames(vchName, 100000000, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    // compile regex once
    using namespace boost::xpressive;
    smatch nameparts;
    sregex cregex = sregex::compile(strRegexp);

    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        string name = stringFromVch(pairScan.first);

        // regexp
        if(strRegexp != "" && !regex_search(name, nameparts, cregex))
            continue;

        CNameIndex txName = pairScan.second;
        int nHeight = txName.nHeight;

        // max age
        if(nMaxAge != 0 && pindexBest->nHeight - nHeight >= nMaxAge)
            continue;

        // from limits
        nCountFrom++;
        if(nCountFrom < nFrom + 1)
            continue;

        Object oName;
        if (!fStat) {
            oName.push_back(Pair("name", name));
            int nExpiresIn = nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight;
            if (nExpiresIn <= 0)
            {
                oName.push_back(Pair("expired", 1));
            }
            else
            {
                string value = stringFromVch(txName.vValue);
                oName.push_back(Pair("value", value));
                oName.push_back(Pair("expires_in", nExpiresIn));
            }
        }
        oRes.push_back(oName);

        nCountNb++;
        // nb limits
        if(nNb > 0 && nCountNb >= nNb)
            break;
    }

    if (NAME_DEBUG) {
        dbName.test();
    }

    if (fStat)
    {
        Object oStat;
        oStat.push_back(Pair("blocks",    (int)nBestHeight));
        oStat.push_back(Pair("count",     (int)oRes.size()));
        //oStat.push_back(Pair("sha256sum", SHA256(oRes), true));
        return oStat;
    }

    return oRes;
}

Value name_scan(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
                "name_scan [<start-name>] [<max-returned>]\n"
                "scan all names, starting at start-name and returning a maximum number of entries (default 500)\n"
                );

    vector<unsigned char> vchName;
    int nMax = 500;
    if (params.size() > 0)
    {
        vchName = vchFromValue(params[0]);
    }

    if (params.size() > 1)
    {
        Value vMax = params[1];
        ConvertTo<double>(vMax);
        nMax = (int)vMax.get_real();
    }

    CNameDB dbName("r");
    Array oRes;

    //vector<pair<vector<unsigned char>, CDiskTxPos> > nameScan;
    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!dbName.ScanNames(vchName, nMax, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    //pair<vector<unsigned char>, CDiskTxPos> pairScan;
    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        Object oName;
        string name = stringFromVch(pairScan.first);
        oName.push_back(Pair("name", name));
        //vector<unsigned char> vchValue;
        CTransaction tx;
        CNameIndex txName = pairScan.second;
        CDiskTxPos txPos = txName.txPos;
        //CDiskTxPos txPos = pairScan.second;
        //int nHeight = GetTxPosHeight(txPos);
        int nHeight = txName.nHeight;
        vector<unsigned char> vchValue = txName.vValue;
        if ((nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
            || txPos.IsNull()
            || !tx.ReadFromDisk(txPos))
            //|| !GetValueOfNameTx(tx, vchValue))
        {
            oName.push_back(Pair("expired", 1));
        }
        else
        {
            string value = stringFromVch(vchValue);
            //string strAddress = "";
            //GetNameAddress(tx, strAddress);
            oName.push_back(Pair("value", value));
            //oName.push_back(Pair("txid", tx.GetHash().GetHex()));
            //oName.push_back(Pair("address", strAddress));
            oName.push_back(Pair("expires_in", nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
        }
        oRes.push_back(oName);
    }

    if (NAME_DEBUG) {
        dbName.test();
    }
    return oRes;
}

//new version
Value name_new(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
                "name_new <name> <value>"
                + HelpRequiringPassphrase());
    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue = vchFromValue(params[1]);
    uint64 rand = GetRand((uint64)-1);
    vector<unsigned char> vchRand = CBigNum(rand).getvch();

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;

    {
        LOCK(cs_main);
        if (mapNamePending.count(vchName) && mapNamePending[vchName].size())
        {

            error("name_firstupdate() : there are %d pending operations on that name, including %s",
                    mapNamePending[vchName].size(),
                    mapNamePending[vchName].begin()->GetHex().c_str());
            throw runtime_error("there are pending operations on that name");
        }
    }

    {
        CNameDB dbName("r");
        CTransaction tx;
        if (GetTxOfName(dbName, vchName, tx))
        {
            error("name_new() : this name is already active with tx %s",
                    tx.GetHash().GetHex().c_str());
            throw runtime_error("this name is already active");
        }
    }

    {
        LOCK(cs_main);
        EnsureWalletIsUnlocked();

        CPubKey vchPubKey;
        if(!pwalletMain->GetKeyFromPool(vchPubKey, true))
        {
            //TODO add actions on failure
        }
        CScript scriptPubKeyOrig;
        scriptPubKeyOrig.SetDestination(vchPubKey.GetID());
        CScript scriptPubKey;
        scriptPubKey << OP_NAME_FIRSTUPDATE << vchName << vchRand << vchValue << OP_2DROP << OP_2DROP;
        scriptPubKey += scriptPubKeyOrig;

        int64 nNetFee = GetNetworkFee(pindexBest->nHeight);
        // Round up to CENT
        nNetFee += CENT - 1;
        nNetFee = (nNetFee / CENT) * CENT;
        string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    return wtx.GetHash().GetHex();
}

Value name_update(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw runtime_error(
                "name_update <name> <value> [<toaddress>]\nUpdate and possibly transfer a name"
                + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue = vchFromValue(params[1]);

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;
    CScript scriptPubKeyOrig;

    if (params.size() == 3)
    {
        string strAddress = params[2].get_str();
        CBitcoinAddress address(strAddress);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid namecoin address");
        scriptPubKeyOrig.SetDestination(address.Get());
    }
    else
    {
        CPubKey vchPubKey;
        if(!pwalletMain->GetKeyFromPool(vchPubKey, true))
        {
            //TODO add actions on failure
        }
        scriptPubKeyOrig.SetDestination(vchPubKey.GetID());
    }

    CScript scriptPubKey;
    scriptPubKey << OP_NAME_UPDATE << vchName << vchValue << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        if (mapNamePending.count(vchName) && mapNamePending[vchName].size())
        {
            error("name_update() : there are %d pending operations on that name, including %s",
                    mapNamePending[vchName].size(),
                    mapNamePending[vchName].begin()->GetHex().c_str());
            throw runtime_error("there are pending operations on that name");
        }

        EnsureWalletIsUnlocked();

        CNameDB dbName("r");
        CTransaction tx;
        if (!GetTxOfName(dbName, vchName, tx))
        {
            throw runtime_error("could not find a coin with this name");
        }

        uint256 wtxInHash = tx.GetHash();

        if (!pwalletMain->mapWallet.count(wtxInHash))
        {
            error("name_update() : this coin is not in your wallet %s",
                    wtxInHash.GetHex().c_str());
            throw runtime_error("this coin is not in your wallet");
        }

        CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
        string strError = SendMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, 0, wtxIn, wtx, false);
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    return wtx.GetHash().GetHex();
}

void UnspendInputs(CWalletTx& wtx)
{
    set<CWalletTx*> setCoins;
    BOOST_FOREACH(const CTxIn& txin, wtx.vin)
    {
        if (!pwalletMain->IsMine(txin))
        {
            printf("UnspendInputs(): !mine %s", txin.ToString().c_str());
            continue;
        }
        CWalletTx& prev = pwalletMain->mapWallet[txin.prevout.hash];
        int nOut = txin.prevout.n;

        printf("UnspendInputs(): %s:%d spent %d\n", prev.GetHash().ToString().c_str(), nOut, prev.IsSpent(nOut));

        if (nOut >= prev.vout.size())
            throw runtime_error("CWalletTx::MarkSpent() : nOut out of range");
        prev.vfSpent.resize(prev.vout.size());
        if (prev.vfSpent[nOut])
        {
            prev.vfSpent[nOut] = false;
            prev.fAvailableCreditCached = false;
            prev.WriteToDisk();
        }
#ifdef GUI
        //pwalletMain->vWalletUpdated.push_back(prev.GetHash());
        pwalletMain->NotifyTransactionChanged(pwalletMain, prev.GetHash(), CT_DELETED);

#endif
    }
}

Value deletetransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
                "deletetransaction <txid>\nNormally used when a transaction cannot be confirmed due to a double spend.\nRestart the program after executing this call.\n"
                );

    if (params.size() != 1)
      throw runtime_error("missing txid");
    {
      LOCK2(cs_main, pwalletMain->cs_wallet);
      uint256 hash;
      hash.SetHex(params[0].get_str());
      if (!pwalletMain->mapWallet.count(hash))
        throw runtime_error("transaction not in wallet");

      if (!mempool.mapTx.count(hash))
      {
        //throw runtime_error("transaction not in memory - is already in blockchain?");
        CTransaction tx;
        uint256 hashBlock = 0;
        if (GetTransaction(hash, tx, hashBlock /*, true*/) && hashBlock != 0)
          throw runtime_error("transaction is already in blockchain");
      }
      CWalletTx wtx = pwalletMain->mapWallet[hash];
      UnspendInputs(wtx);

      // We are not removing from mapTransactions because this can cause memory corruption
      // during mining.  The user should restart to clear the tx from memory.
      mempool.remove(wtx);
      pwalletMain->EraseFromWallet(wtx.GetHash());
      vector<unsigned char> vchName;
      if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName)) {
        printf("deletetransaction() : remove from pending");
        mapNamePending[vchName].erase(wtx.GetHash());
      }
      return "success, please restart program to clear memory";
    }
}

Value name_clean(const Array& params, bool fHelp)
{
    if (fHelp || params.size())
        throw runtime_error("name_clean\nClean unsatisfiable transactions from the wallet - including name_update on an already taken name\n");

    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        map<uint256, CWalletTx> mapRemove;

        printf("-----------------------------\n");

        {
            CTxDB txdb("r");
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
            {
                CWalletTx& wtx = item.second;
                vector<unsigned char> vchName;
                if (wtx.GetDepthInMainChain() < 1 && IsConflictedTx(txdb, wtx, vchName))
                {
                    uint256 hash = wtx.GetHash();
                    mapRemove[hash] = wtx;
                }
            }
        }

        bool fRepeat = true;
        while (fRepeat)
        {
            fRepeat = false;
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
            {
                CWalletTx& wtx = item.second;
                BOOST_FOREACH(const CTxIn& txin, wtx.vin)
                {
                    uint256 hash = wtx.GetHash();

                    // If this tx depends on a tx to be removed, remove it too
                    if (mapRemove.count(txin.prevout.hash) && !mapRemove.count(hash))
                    {
                        mapRemove[hash] = wtx;
                        fRepeat = true;
                    }
                }
            }
        }

        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapRemove)
        {
            CWalletTx& wtx = item.second;

            UnspendInputs(wtx);
            mempool.remove(wtx);
            pwalletMain->EraseFromWallet(wtx.GetHash());
            vector<unsigned char> vchName;
            if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName))
            {
                string name = stringFromVch(vchName);
                printf("name_clean() : erase %s from pending of name %s",
                        wtx.GetHash().GetHex().c_str(), name.c_str());
                if (!mapNamePending[vchName].erase(wtx.GetHash()))
                    error("name_clean() : erase but it was not pending");
            }
            wtx.print();
        }

        printf("-----------------------------\n");
    }

    return true;
}

// Check that the last entry in name history matches the given tx pos
bool CheckNameTxPos(const vector<CNameIndex> &vtxPos, const CDiskTxPos& txPos)
{
    if (vtxPos.empty())
        return false;

    return vtxPos.back().txPos == txPos;
}

bool DecodeNameTx(const CTransaction& tx, int& op, int& nOut, vector<vector<unsigned char> >& vvch)
{
    bool found = false;

    // Strict check - bug disallowed
    for (int i = 0; i < tx.vout.size(); i++)
    {
        const CTxOut& out = tx.vout[i];

        vector<vector<unsigned char> > vvchRead;

        if (DecodeNameScript(out.scriptPubKey, op, vvchRead))
        {
            // If more than one name op, fail
            if (found)
            {
                vvch.clear();
                return false;
            }
            nOut = i;
            found = true;
            vvch = vvchRead;
        }
    }

    if (!found)
        vvch.clear();


    return found;
}

int64 GetNameNetFee(const CTransaction& tx)
{
    int64 nFee = 0;

    for (int i = 0 ; i < tx.vout.size() ; i++)
    {
        const CTxOut& out = tx.vout[i];
        if (out.scriptPubKey.size() == 1 && out.scriptPubKey[0] == OP_RETURN)
        {
            nFee += out.nValue;
        }
    }

    return nFee;
}

bool GetValueOfNameTx(const CTransaction& tx, vector<unsigned char>& value)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    if (!DecodeNameTx(tx, op, nOut, vvch))
        return false;

    switch (op)
    {
        case OP_NAME_NEW:
            return false;
        case OP_NAME_FIRSTUPDATE:
            value = vvch[2];
            return true;
        case OP_NAME_UPDATE:
            value = vvch[1];
            return true;
        default:
            return false;
    }
}

int IndexOfNameOutput(const CTransaction& tx)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
        throw runtime_error("IndexOfNameOutput() : name output not found");
    return nOut;
}

void CNamecoinHooks::AddToWallet(CWalletTx& wtx)
{
}

bool CNamecoinHooks::IsMine(const CTransaction& tx)
{
    if (fDebug) printf("CNamecoinHooks::IsMine\n");
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    // We do the check under the correct rule set (post-hardfork)
    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
    {
        error("IsMine() hook : no output out script in name tx %s\n", tx.ToString().c_str());
        return false;
    }

    const CTxOut& txout = tx.vout[nOut];
    if (IsMyName(tx, txout))
    {
        printf("IsMine() hook : found my transaction %s nout %d\n", tx.GetHash().GetHex().c_str(), nOut);
        return true;
    }
    return false;
}

bool CNamecoinHooks::IsMine(const CTransaction& tx, const CTxOut& txout, bool ignore_name_new /* = false*/)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;

    vector<vector<unsigned char> > vvch;

    int op;
    //int nOut;

    if (!DecodeNameScript(txout.scriptPubKey, op, vvch))
        return false;

    if (ignore_name_new && op == OP_NAME_NEW)
        return false;

    if (IsMyName(tx, txout))
    {
        printf("IsMine() hook : found my transaction %s value %ld\n", tx.GetHash().GetHex().c_str(), txout.nValue);
        return true;
    }
    return false;
}

void CNamecoinHooks::AcceptToMemoryPool(CTxDB& txdb, const CTransaction& tx)
{
    if (fDebug) printf("CNamecoinHooks::AcceptToMemoryPool\n");
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return;

    if (tx.vout.size() < 1)
    {
        error("AcceptToMemoryPool() : no output in name tx %s\n", tx.ToString().c_str());
        return;
    }

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
    {
        error("AcceptToMemoryPool() : no output out script in name tx %s", tx.ToString().c_str());
        return;
    }

    {
        LOCK(cs_main);
        if (op != OP_NAME_NEW)
        {
            mapNamePending[vvch[0]].insert(tx.GetHash());
        }
    }
}

int CheckTransactionAtRelativeDepth(const CBlockIndex* pindexBlock, CTxIndex& txindex, int maxDepth)
{
    if (pindexBlock->nBlockPos == txindex.pos.nBlockPos && pindexBlock->nFile == txindex.pos.nFile)
        return 0; //we do this first check outside loop because of const keyword

    for (CBlockIndex* pindex = pindexBlock->pprev; pindex && pindexBlock->nHeight - pindex->nHeight < maxDepth; pindex = pindex->pprev)
        if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
            return pindexBlock->nHeight - pindex->nHeight;
    return -1;
}

bool GetNameOfTx(const CTransaction& tx, vector<unsigned char>& name)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;
    vector<vector<unsigned char> > vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("GetNameOfTx() : could not decode a namecoin tx");

    switch (op)
    {
        case OP_NAME_FIRSTUPDATE:
        case OP_NAME_UPDATE:
            name = vvchArgs[0];
            return true;
    }
    return false;
}

bool IsConflictedTx(CTxDB& txdb, const CTransaction& tx, vector<unsigned char>& name)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;
    vector<vector<unsigned char> > vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("IsConflictedTx() : could not decode a namecoin tx");
    int nPrevHeight;
    //int nDepth;
    //int64 nNetFee;

    switch (op)
    {
        case OP_NAME_FIRSTUPDATE:
            nPrevHeight = GetNameHeight(txdb, vvchArgs[0]);
            name = vvchArgs[0];
            if (nPrevHeight >= 0 && pindexBest->nHeight - nPrevHeight < GetExpirationDepth(pindexBest->nHeight))
                return true;
    }
    return false;
}

bool CNamecoinHooks::ConnectInputs(CTxDB& txdb,
        map<uint256, CTxIndex>& mapTestPool,
        const CTransaction& tx,
        vector<CTransaction>& vTxPrev, //vector of all input transactions
        vector<CTxIndex>& vTxindex,
        const CBlockIndex* pindexBlock,
        const CDiskTxPos& txPos,
        bool fBlock,
        bool fMiner)
{
    if (fDebug) {
        printf("\nCNamecoinHooks::ConnectInputs: fBlock = %d, fMiner = %d\n, nHeight = %d", fBlock, fMiner, pindexBlock->nHeight);
        printf(tx.ToString().c_str());
        for (int i = 0; i < vTxPrev.size(); i++){
            printf(vTxPrev[i].ToString().c_str());
        }
        printf("\n");
    }


    int nInput;
    bool found = false;

    int prevOp;
    vector<vector<unsigned char> > vvchPrevArgs;

    {
        // Strict check - bug disallowed
        if (fDebug) printf("strict check\n\n");
        for (int i = 0; i < tx.vin.size(); i++)
        {
            CTxOut& out = vTxPrev[i].vout[tx.vin[i].prevout.n];
            if (fDebug) printf(out.ToString().c_str());

            vector<vector<unsigned char> > vvchPrevArgsRead;

            if (DecodeNameScript(out.scriptPubKey, prevOp, vvchPrevArgsRead))
            {
                if (found)
                    return error("ConnectInputHook() : multiple previous name transactions");
                found = true;
                nInput = i;

                vvchPrevArgs = vvchPrevArgsRead;
            }
        }
    }

    if (tx.nVersion != NAMECOIN_TX_VERSION)
    {
        // Make sure name-op outputs are not spent by a regular transaction, or the name
        // would be lost
        if (found)
            return error("ConnectInputHook() : a non-namecoin transaction with a namecoin input");
        return true;
    }

    vector<vector<unsigned char> > vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("ConnectInputsHook() : could not decode a namecoin tx");

    int nPrevHeight;
    int nDepth;
//    int64 nNetFee;

    bool fBugWorkaround = false;

    // HACK: The following two checks are redundant after hard-fork at block 150000, because it is performed
    // in CheckTransaction. However, before that, we do not know height during CheckTransaction
    // and cannot apply the right set of rules
    if (vvchArgs[0].size() > MAX_NAME_LENGTH)
        return error("name transaction with name too long");

    if (fDebug) {
        string s = stringFromVch(vvchArgs[0]);
        printf("vvchArgs[0].size() = %d, string = %s, ", vvchArgs[0].size(), s.c_str());
    }

    switch (op)
    {
        case OP_NAME_NEW:
            if (fDebug) printf("op = OP_NAME_NEW, ");
            return false;
        case OP_NAME_FIRSTUPDATE:
            if (fDebug) printf("op = OP_NAME_FIRSTUPDATE, ");
//            nNetFee = GetNameNetFee(tx);
//            if (nNetFee < GetNetworkFee(pindexBlock->nHeight))
//                return error("ConnectInputsHook() : got tx %s with fee too low %d", tx.GetHash().GetHex().c_str(), nNetFee);

            nPrevHeight = GetNameHeight(txdb, vvchArgs[0]);
            if (nPrevHeight >= 0 && pindexBlock->nHeight - nPrevHeight < GetExpirationDepth(pindexBlock->nHeight))
                return error("ConnectInputsHook() : name_firstupdate on an unexpired name");

            if (fMiner)
            {
                // Check that no other pending txs on this name are already in the block to be mined
                set<uint256>& setPending = mapNamePending[vvchArgs[0]];
                BOOST_FOREACH(const PAIRTYPE(uint256, CTxIndex)& s, mapTestPool)
                {
                    if (setPending.count(s.first))
                    {
                        printf("ConnectInputsHook() : will not mine %s because it clashes with %s",
                                tx.GetHash().GetHex().c_str(),
                                s.first.GetHex().c_str());
                        return false;
                    }
                }
            }
            break;
        case OP_NAME_UPDATE:
            if (fDebug) {
                printf("op = OP_NAME_UPDATE, ");
                if (!found)
                    printf("!found == true ");
                if (prevOp != OP_NAME_FIRSTUPDATE)
                    printf("(prevOp != OP_NAME_FIRSTUPDATE) == true ");
                if (prevOp != OP_NAME_UPDATE)
                    printf("(prevOp != OP_NAME_UPDATE) == true, ");
            }


            if (!found || (prevOp != OP_NAME_FIRSTUPDATE && prevOp != OP_NAME_UPDATE))
                return error("name_update tx without previous update tx");

            // HACK: The following check is redundant after hard-fork at block 150000, because it is performed
            // in CheckTransaction. However, before that, we do not know height during CheckTransaction
            // and cannot apply the right set of rules
            if (vvchArgs[1].size() > MAX_VALUE_LENGTH)
                return error("name_update tx with value too long");

            // Check name
            if (vvchPrevArgs[0] != vvchArgs[0])
            {
                if (pindexBlock->nHeight >= BUG_WORKAROUND_BLOCK)
                    return error("ConnectInputsHook() : name_update name mismatch");
                else
                {
                    // Accept bad transactions before the hard-fork point, but do not write them to name DB
                    printf("ConnectInputsHook() : name_update mismatch bug workaround");
                    fBugWorkaround = true;
                }
            }

            // TODO CPU intensive
            nDepth = CheckTransactionAtRelativeDepth(pindexBlock, vTxindex[nInput], GetExpirationDepth(pindexBlock->nHeight));
            if ((fBlock || fMiner) && nDepth < 0)
                return error("ConnectInputsHook() : name_update on an expired name, or there is a pending transaction on the name");
            break;
        default:
            return error("ConnectInputsHook() : name transaction has unknown op");
    }
    if (fDebug) printf("checkpoint 1, ");

    if (!fBugWorkaround)
    {
        if (fDebug) printf("checkpoint 2, ");
        CNameDB dbName("cr+", txdb);
        dbName.TxnBegin();

        if (!fBlock && op == OP_NAME_UPDATE)
        {
            vector<CNameIndex> vtxPos;
            if (dbName.ExistsName(vvchArgs[0]))
            {
                if (!dbName.ReadName(vvchArgs[0], vtxPos))
                    return error("ConnectInputsHook() : failed to read from name DB");
            }
            // Valid tx on top of buggy tx: if not in block, reject
            if (!CheckNameTxPos(vtxPos, vTxindex[nInput].pos))
                return error("ConnectInputsHook() : Name bug workaround: tx %s rejected, since previous tx (%s) is not in the name DB\n", tx.GetHash().ToString().c_str(), vTxPrev[nInput].GetHash().ToString().c_str());
        }

        if (fBlock)
        {
            if (op == OP_NAME_FIRSTUPDATE || op == OP_NAME_UPDATE)
            {
                if (fDebug) printf("checkpoint 3, ");
                vector<CNameIndex> vtxPos;
                if (dbName.ExistsName(vvchArgs[0]))
                {
                    if (!dbName.ReadName(vvchArgs[0], vtxPos))
                        return error("ConnectInputsHook() : failed to read from name DB");
                }

                if (op == OP_NAME_UPDATE && !CheckNameTxPos(vtxPos, vTxindex[nInput].pos))
                {
                    if (fDebug) printf("checkpoint 4, ");
                    printf("ConnectInputsHook() : Name bug workaround: tx %s rejected, since previous tx (%s) is not in the name DB\n", tx.GetHash().ToString().c_str(), vTxPrev[nInput].GetHash().ToString().c_str());
                    // Valid tx on top of buggy tx: reject only after hard-fork
                    if (pindexBlock->nHeight >= BUG_WORKAROUND_BLOCK)
                        return false;
                    else
                        fBugWorkaround = true;
                }

                if (!fBugWorkaround)
                {
                    if (fDebug) printf("checkpoint 5, ");
                    vector<unsigned char> vchValue; // add
                    int nHeight;
                    uint256 hash;
                    GetValueOfTxPos(txPos, vchValue, hash, nHeight);
                    CNameIndex txPos2;
                    txPos2.nHeight = pindexBlock->nHeight;
                    txPos2.vValue = vchValue;
                    txPos2.txPos = txPos;
                    vtxPos.push_back(txPos2); // fin add
                    if (!dbName.WriteName(vvchArgs[0], vtxPos))
                        return error("ConnectInputsHook() : failed to write to name DB");
                }
            }


            if (op != OP_NAME_NEW)
            {
                if (fDebug) printf("checkpoint 6, ");
                LOCK(cs_main);
                std::map<std::vector<unsigned char>, std::set<uint256> >::iterator mi = mapNamePending.find(vvchArgs[0]);
                if (mi != mapNamePending.end())
                    mi->second.erase(tx.GetHash());
            }
        }
        dbName.TxnCommit();
    }
    if (fDebug) printf("end!");

    return true;
}

bool CNamecoinHooks::DisconnectInputs(CTxDB& txdb,
        const CTransaction& tx,
        CBlockIndex* pindexBlock)
{
    if (fDebug) printf("CNamecoinHooks::CheckTransaction\n");
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    vector<vector<unsigned char> > vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("DisconnectInputsHook() : could not decode namecoin tx");
    if (op == OP_NAME_FIRSTUPDATE || op == OP_NAME_UPDATE)
    {
        CNameDB dbName("cr+", txdb);

        dbName.TxnBegin();

        //vector<CDiskTxPos> vtxPos;
        vector<CNameIndex> vtxPos;
        if (!dbName.ReadName(vvchArgs[0], vtxPos))
            return error("DisconnectInputsHook() : failed to read from name DB");
        // vtxPos might be empty if we pruned expired transactions.  However, it should normally still not
        // be empty, since a reorg cannot go that far back.  Be safe anyway and do not try to pop if empty.
        if (vtxPos.size())
        {
            CTxIndex txindex;
            if (!txdb.ReadTxIndex(tx.GetHash(), txindex))
                return error("DisconnectInputsHook() : failed to read tx index");

            if (vtxPos.back().txPos == txindex.pos)
                vtxPos.pop_back();

            // TODO validate that the first pos is the current tx pos
        }
        if (!dbName.WriteName(vvchArgs[0], vtxPos))
            return error("DisconnectInputsHook() : failed to write to name DB");

        dbName.TxnCommit();
    }

    return true;
}

bool CNamecoinHooks::CheckTransaction(const CTransaction& tx)
{
    if (fDebug) printf("CNamecoinHooks::CheckTransaction\n");
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    vector<vector<unsigned char> > vvch;
    int op;
    int nOut;

    // HACK: We do not know height here, so we check under both old and new rule sets (before/after hardfork)
    // The correct check is duplicated in ConnectInputs.
    bool ret[2];
    for (int iter = 0; iter < 2; iter++)
    {
        ret[iter] = true;

        bool good = DecodeNameTx(tx, op, nOut, vvch);

        if (!good)
        {
            ret[iter] = error("name transaction has unknown script format");
            continue;
        }

        if (vvch[0].size() > MAX_NAME_LENGTH)
        {
            ret[iter] = error("name transaction with name too long");
            continue;
        }

        switch (op)
        {
            case OP_NAME_NEW:
                if (vvch[0].size() != 20)
                    ret[iter] = error("name_new tx with incorrect hash length");
                break;
            case OP_NAME_FIRSTUPDATE:
                if (vvch[1].size() > 20)
                    ret[iter] = error("name_firstupdate tx with rand too big");
                if (vvch[2].size() > MAX_VALUE_LENGTH)
                    ret[iter] = error("name_firstupdate tx with value too long");
                break;
            case OP_NAME_UPDATE:
                if (vvch[1].size() > MAX_VALUE_LENGTH)
                    ret[iter] = error("name_update tx with value too long");
                break;
            default:
                ret[iter] = error("name transaction has unknown op");
        }
    }
    return ret[0] || ret[1];
}

static string nameFromOp(int op)
{
    switch (op)
    {
        case OP_NAME_NEW:
            return "name_new";
        case OP_NAME_UPDATE:
            return "name_update";
        case OP_NAME_FIRSTUPDATE:
            return "name_firstupdate";
        default:
            return "<unknown name op>";
    }
}

bool CNamecoinHooks::ExtractAddress(const CScript& script, string& address)
{
    if (fDebug) printf("CNamecoinHooks::ExtractAddress\n");
    if (script.size() == 1 && script[0] == OP_RETURN)
    {
        address = string("network fee");
        return true;
    }
    vector<vector<unsigned char> > vvch;
    int op;
    if (!DecodeNameScript(script, op, vvch))
        return false;

    string strOp = nameFromOp(op);
    string strName;
    if (op == OP_NAME_NEW)
    {
#ifdef GUI
        LOCK(cs_main);

        std::map<uint160, std::vector<unsigned char> >::const_iterator mi = mapMyNameHashes.find(uint160(vvch[0]));
        if (mi != mapMyNameHashes.end())
            strName = stringFromVch(mi->second);
        else
#endif
            strName = HexStr(vvch[0]);
    }
    else
        strName = stringFromVch(vvch[0]);

    address = strOp + ": " + strName;
    return true;
}

bool CNamecoinHooks::ConnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex)
{
    return true;
}

bool CNamecoinHooks::DisconnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex)
{
    return true;
}

bool GenesisBlock(CBlock& block, int extra)
{
    block = CBlock();
    block.hashPrevBlock = 0;
    block.nVersion = 1;
    block.nTime    = 1303000001;
    block.nBits    = 0x1c007fff;
    block.nNonce   = 0xa21ea192U;
    const char* pszTimestamp = "... choose what comes next.  Lives of your own, or a return to chains. -- V";
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << block.nBits << CBigNum(++extra) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = 50 * COIN;
    txNew.vout[0].scriptPubKey = CScript() << ParseHex("04b620369050cd899ffbbc4e8ee51e8c4534a855bb463439d63d235d4779685d8b6f4870a238cf365ac94fa13ef9a2a22cd99d0d5ee86dcabcafce36c7acf43ce5") << OP_CHECKSIG;
    block.vtx.push_back(txNew);
    block.hashMerkleRoot = block.BuildMerkleTree();
    printf("====================================\n");
    printf("Merkle: %s\n", block.hashMerkleRoot.GetHex().c_str());
    printf("Block: %s\n", block.GetHash().GetHex().c_str());
    block.print();
    assert(block.GetHash() == hashGenesisBlock);
    return true;
}

bool CNamecoinHooks::GenesisBlock(CBlock& block)
{
    if (fTestNet)
        return false;

    return ::GenesisBlock(block, NAME_COIN_GENESIS_EXTRA);
}

int CNamecoinHooks::LockinHeight()
{
    if (fTestNet)
        return 0;

    return 112896;
}

bool CNamecoinHooks::Lockin(int nHeight, uint256 hash)
{
    if (!fTestNet)
        if ((nHeight == 2016 && hash != uint256("0x0000000000660bad0d9fbde55ba7ee14ddf766ed5f527e3fbca523ac11460b92")) ||
                (nHeight ==   4032 && hash != uint256("0x0000000000493b5696ad482deb79da835fe2385304b841beef1938655ddbc411")) ||
                (nHeight ==   6048 && hash != uint256("0x000000000027939a2e1d8bb63f36c47da858e56d570f143e67e85068943470c9")) ||
                (nHeight ==   8064 && hash != uint256("0x000000000003a01f708da7396e54d081701ea406ed163e519589717d8b7c95a5")) ||
                (nHeight ==  10080 && hash != uint256("0x00000000000fed3899f818b2228b4f01b9a0a7eeee907abd172852df71c64b06")) ||
                (nHeight ==  12096 && hash != uint256("0x0000000000006c06988ff361f124314f9f4bb45b6997d90a7ee4cedf434c670f")) ||
                (nHeight ==  14112 && hash != uint256("0x00000000000045d95e0588c47c17d593c7b5cb4fb1e56213d1b3843c1773df2b")) ||
                (nHeight ==  16128 && hash != uint256("0x000000000001d9964f9483f9096cf9d6c6c2886ed1e5dec95ad2aeec3ce72fa9")) ||
                (nHeight ==  18940 && hash != uint256("0x00000000000087f7fc0c8085217503ba86f796fa4984f7e5a08b6c4c12906c05")) ||
                (nHeight ==  30240 && hash != uint256("0xe1c8c862ff342358384d4c22fa6ea5f669f3e1cdcf34111f8017371c3c0be1da")) ||
                (nHeight ==  57000 && hash != uint256("0xaa3ec60168a0200799e362e2b572ee01f3c3852030d07d036e0aa884ec61f203")) ||
                (nHeight == 112896 && hash != uint256("0x73f880e78a04dd6a31efc8abf7ca5db4e262c4ae130d559730d6ccb8808095bf")))
            return false;
    return true;
}

string CNamecoinHooks::IrcPrefix()
{
    return "namecoin";
}