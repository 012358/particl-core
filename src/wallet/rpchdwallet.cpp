// Copyright (c) 2017 The Particl Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "base58.h"
#include "chain.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "init.h"
#include "validation.h"
#include "net.h"
#include "policy/policy.h"
#include "policy/rbf.h"
#include "rpc/server.h"
#include "script/sign.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#include "hdwallet.h"
#include "hdwalletdb.h"
#include "chainparams.h"
#include "key/mnemonic.h"
#include "pos/miner.h"
#include "crypto/sha256.h"

#include <stdint.h>

#include <boost/assign/list_of.hpp>

#include <univalue.h>

void EnsureWalletIsUnlocked(CHDWallet *pwallet)
{
    if (pwallet->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet locked, please enter the wallet passphrase with walletpassphrase first.");
    
    if (pwallet->fUnlockForStakingOnly)
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet is unlocked for staking only.");
}

static CHDWallet *GetHDWallet()
{
    CHDWallet *rv;
    if (!pwalletMain)
        throw std::runtime_error("pwalletMain is null.");
    if (!(rv = dynamic_cast<CHDWallet*>(pwalletMain)))
        throw std::runtime_error("pwalletMain is not an instance of class CHDWallet.");
    return rv;
};

inline uint32_t reversePlace(uint8_t *p)
{
    uint32_t rv = 0;
    for (int i = 0; i < 4; ++i)
        rv |= (uint32_t) *(p+i) << (8 * (3-i));
    return rv;
};

int ExtractBip32InfoV(std::vector<unsigned char> &vchKey, UniValue &keyInfo, std::string &sError)
{
    CExtKey58 ek58;
    CExtKeyPair vk;
    vk.DecodeV(&vchKey[4]);
    
    CChainParams::Base58Type typePk = CChainParams::EXT_PUBLIC_KEY;
    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY)[0], 4) == 0)
        keyInfo.push_back(Pair("type", "Particl extended secret key"));
    else
    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY_BTC)[0], 4) == 0)
    {
        keyInfo.push_back(Pair("type", "Bitcoin extended secret key"));
        typePk = CChainParams::EXT_PUBLIC_KEY_BTC;
    } else
        keyInfo.push_back(Pair("type", "Unknown extended secret key"));
    
    keyInfo.push_back(Pair("version", strprintf("%02X", reversePlace(&vchKey[0]))));
    keyInfo.push_back(Pair("depth", strprintf("%u", vchKey[4])));
    keyInfo.push_back(Pair("parent_fingerprint", strprintf("%08X", reversePlace(&vchKey[5]))));
    keyInfo.push_back(Pair("child_index", strprintf("%u", reversePlace(&vchKey[9]))));
    keyInfo.push_back(Pair("chain_code", strprintf("%s", HexStr(&vchKey[13], &vchKey[13+32]))));
    keyInfo.push_back(Pair("key", strprintf("%s", HexStr(&vchKey[46], &vchKey[46+32]))));
    
    // don't display raw secret ??
    // TODO: add option
    
    CKey key;
    key.Set(&vchKey[46], true);
    keyInfo.push_back(Pair("privkey", strprintf("%s", CBitcoinSecret(key).ToString())));
    CKeyID id = key.GetPubKey().GetID();
    CBitcoinAddress addr;
    addr.Set(id, CChainParams::EXT_KEY_HASH);
    
    keyInfo.push_back(Pair("id", addr.ToString().c_str()));
    addr.Set(id);
    keyInfo.push_back(Pair("address", addr.ToString().c_str()));
    keyInfo.push_back(Pair("checksum", strprintf("%02X", reversePlace(&vchKey[78]))));
    
    ek58.SetKey(vk, typePk);
    keyInfo.push_back(Pair("ext_public_key", ek58.ToString()));
    
    return 0;
};

int ExtractBip32InfoP(std::vector<unsigned char> &vchKey, UniValue &keyInfo, std::string &sError)
{
    CExtPubKey pk;
    
    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY)[0], 4) == 0)
        keyInfo.push_back(Pair("type", "Particl extended public key"));
    else
    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY_BTC)[0], 4) == 0)
        keyInfo.push_back(Pair("type", "Bitcoin extended public key"));
    else
        keyInfo.push_back(Pair("type", "Unknown extended public key"));
        
    keyInfo.push_back(Pair("version", strprintf("%02X", reversePlace(&vchKey[0]))));
    keyInfo.push_back(Pair("depth", strprintf("%u", vchKey[4])));
    keyInfo.push_back(Pair("parent_fingerprint", strprintf("%08X", reversePlace(&vchKey[5]))));
    keyInfo.push_back(Pair("child_index", strprintf("%u", reversePlace(&vchKey[9]))));
    keyInfo.push_back(Pair("chain_code", strprintf("%s", HexStr(&vchKey[13], &vchKey[13+32]))));
    keyInfo.push_back(Pair("key", strprintf("%s", HexStr(&vchKey[45], &vchKey[45+33]))));
    
    CPubKey key;
    key.Set(&vchKey[45], &vchKey[78]);
    CKeyID id = key.GetID();
    CBitcoinAddress addr;
    addr.Set(id, CChainParams::EXT_KEY_HASH);
    
    keyInfo.push_back(Pair("id", addr.ToString().c_str()));
    addr.Set(id);
    keyInfo.push_back(Pair("address", addr.ToString().c_str()));
    keyInfo.push_back(Pair("checksum", strprintf("%02X", reversePlace(&vchKey[78]))));
    
    return 0;
};

int ExtKeyPathV(std::string &sPath, std::vector<uint8_t> &vchKey, UniValue &keyInfo, std::string &sError)
{
    if (sPath.compare("info") == 0)
        return ExtractBip32InfoV(vchKey, keyInfo, sError);
    
    CExtKey vk;
    vk.Decode(&vchKey[4]);
    
    CExtKey vkOut;
    CExtKey vkWork = vk;
    
    std::vector<uint32_t> vPath;
    int rv;
    if ((rv = ExtractExtKeyPath(sPath, vPath)) != 0)
    {
        sError = ExtKeyGetString(rv);
        return 1;
    };
    
    for (std::vector<uint32_t>::iterator it = vPath.begin(); it != vPath.end(); ++it)
    {
        if (*it == 0)
        {
            vkOut = vkWork;
        } else
        if (!vkWork.Derive(vkOut, *it))
        {
            sError = "CExtKey Derive failed.";
            return 1;
        };
        vkWork = vkOut;
    };
    
    CBitcoinExtKey ekOut;
    ekOut.SetKey(vkOut);
    keyInfo.push_back(Pair("result", ekOut.ToString()));
    
    return 0;
};

int ExtKeyPathP(std::string &sPath, std::vector<uint8_t> &vchKey, UniValue &keyInfo, std::string &sError)
{
    if (sPath.compare("info") == 0)
        return ExtractBip32InfoP(vchKey, keyInfo, sError);
    
    CExtPubKey pk;
    pk.Decode(&vchKey[4]);
    
    CExtPubKey pkOut;
    CExtPubKey pkWork = pk;
    
    std::vector<uint32_t> vPath;
    int rv;
    if ((rv = ExtractExtKeyPath(sPath, vPath)) != 0)
    {
        sError = ExtKeyGetString(rv);
        return 1;
    };
    
    for (std::vector<uint32_t>::iterator it = vPath.begin(); it != vPath.end(); ++it)
    {
        if (*it == 0)
        {
            pkOut = pkWork;
        } else
        if ((*it >> 31) == 1)
        {
            sError = "Can't derive hardened keys from public ext key.";
            return 1;
        } else
        if (!pkWork.Derive(pkOut, *it))
        {
            sError = "CExtKey Derive failed.";
            return 1;
        };
        pkWork = pkOut;
    };
    
    CBitcoinExtPubKey ekOut;
    ekOut.SetKey(pkOut);
    keyInfo.push_back(Pair("result", ekOut.ToString()));
    
    return 0;
};

int AccountInfo(CExtKeyAccount *pa, int nShowKeys, bool fAllChains, UniValue &obj, std::string &sError)
{
    CHDWallet *pwallet = (CHDWallet*) pwalletMain;
    CExtKey58 eKey58;
    
    obj.push_back(Pair("type", "Account"));
    obj.push_back(Pair("active", pa->nFlags & EAF_ACTIVE ? "true" : "false"));
    obj.push_back(Pair("label", pa->sLabel));
    
    if (pwallet->idDefaultAccount == pa->GetID())
        obj.push_back(Pair("default_account", "true"));
    
    mapEKValue_t::iterator mvi = pa->mapValue.find(EKVT_CREATED_AT);
    if (mvi != pa->mapValue.end())
    {
        int64_t nCreatedAt;
        GetCompressedInt64(mvi->second, (uint64_t&)nCreatedAt);
        obj.push_back(Pair("created_at", nCreatedAt));
    };
    
    obj.push_back(Pair("id", pa->GetIDString58()));
    obj.push_back(Pair("has_secret", pa->nFlags & EAF_HAVE_SECRET ? "true" : "false"));
    
    CStoredExtKey *sekAccount = pa->ChainAccount();
    if (!sekAccount)
    {
        obj.push_back(Pair("error", "chain account not set."));
        return 0;
    };
    
    CBitcoinAddress addr;
    addr.Set(pa->idMaster, CChainParams::EXT_KEY_HASH);
    obj.push_back(Pair("root_key_id", addr.ToString()));
    
    mvi = sekAccount->mapValue.find(EKVT_PATH);
    if (mvi != sekAccount->mapValue.end())
    {
        std::string sPath;
        if (0 == PathToString(mvi->second, sPath, 'h'))
            obj.push_back(Pair("path", sPath));
    };
    // TODO: separate passwords for accounts
    if (pa->nFlags & EAF_HAVE_SECRET
        && nShowKeys > 1
        && pwallet->ExtKeyUnlock(sekAccount) == 0)
    {
        eKey58.SetKeyV(sekAccount->kp);
        obj.push_back(Pair("evkey", eKey58.ToString()));
    };
    
    if (nShowKeys > 0)
    {
        eKey58.SetKeyP(sekAccount->kp);
        obj.push_back(Pair("epkey", eKey58.ToString()));
    };
    
    if (fAllChains)
    {
        UniValue arChains(UniValue::VARR);
        for (size_t i = 1; i < pa->vExtKeys.size(); ++i) // vExtKeys[0] stores the account key
        {
            UniValue objC(UniValue::VOBJ);
            CStoredExtKey *sek = pa->vExtKeys[i];
            eKey58.SetKeyP(sek->kp);
            
            if (pa->nActiveExternal == i)
                objC.push_back(Pair("function", "active_external"));
            if (pa->nActiveInternal == i)
                objC.push_back(Pair("function", "active_internal"));
            if (pa->nActiveStealth == i)
                objC.push_back(Pair("function", "active_stealth"));
            
            objC.push_back(Pair("id", sek->GetIDString58()));
            objC.push_back(Pair("chain", eKey58.ToString()));
            objC.push_back(Pair("label", sek->sLabel));
            objC.push_back(Pair("active", sek->nFlags & EAF_ACTIVE ? "true" : "false"));
            objC.push_back(Pair("receive_on", sek->nFlags & EAF_RECEIVE_ON ? "true" : "false"));
            objC.push_back(Pair("num_derives", strprintf("%u", sek->nGenerated)));
            objC.push_back(Pair("num_derives_h", strprintf("%u", sek->nHGenerated)));
            
            mvi = sek->mapValue.find(EKVT_PATH);
            if (mvi != sek->mapValue.end())
            {
                std::string sPath;
                if (0 == PathToString(mvi->second, sPath, 'h'))
                    objC.push_back(Pair("path", sPath));
            };
            
            arChains.push_back(objC);
        };
        obj.push_back(Pair("chains", arChains));
    } else
    {
        if (pa->nActiveExternal < pa->vExtKeys.size())
        {
            CStoredExtKey *sekE = pa->vExtKeys[pa->nActiveExternal];
            if (nShowKeys > 0)
            {
                eKey58.SetKeyP(sekE->kp);
                obj.push_back(Pair("external_chain", eKey58.ToString()));
            };
            obj.push_back(Pair("num_derives_external", strprintf("%u", sekE->nGenerated)));
            obj.push_back(Pair("num_derives_external_h", strprintf("%u", sekE->nHGenerated)));
        };
        
        if (pa->nActiveInternal < pa->vExtKeys.size())
        {
            CStoredExtKey *sekI = pa->vExtKeys[pa->nActiveInternal];
            if (nShowKeys > 0)
            {
                eKey58.SetKeyP(sekI->kp);
                obj.push_back(Pair("internal_chain", eKey58.ToString()));
            };
            obj.push_back(Pair("num_derives_internal", strprintf("%u", sekI->nGenerated)));
            obj.push_back(Pair("num_derives_internal_h", strprintf("%u", sekI->nHGenerated)));
        };
        
        if (pa->nActiveStealth < pa->vExtKeys.size())
        {
            CStoredExtKey *sekS = pa->vExtKeys[pa->nActiveStealth];
            obj.push_back(Pair("num_derives_stealth", strprintf("%u", sekS->nGenerated)));
            obj.push_back(Pair("num_derives_stealth_h", strprintf("%u", sekS->nHGenerated)));
        };
    }
    
    return 0;
};

int AccountInfo(CKeyID &keyId, int nShowKeys, bool fAllChains, UniValue &obj, std::string &sError)
{
    CHDWallet *pwallet = (CHDWallet*) pwalletMain;
    // TODO: inactive keys can be in db and not in memory - search db for keyId
    ExtKeyAccountMap::iterator mi = pwallet->mapExtAccounts.find(keyId);
    if (mi == pwallet->mapExtAccounts.end())
    {
        sError = "Unknown account.";
        return 1;
    };
    
    CExtKeyAccount *pa = mi->second;
    
    return AccountInfo(pa, nShowKeys, fAllChains, obj, sError);
};

int KeyInfo(CKeyID &idMaster, CKeyID &idKey, CStoredExtKey &sek, int nShowKeys, UniValue &obj, std::string &sError)
{
    CHDWallet *pwallet = (CHDWallet*) pwalletMain;
    CExtKey58 eKey58;
    
    bool fBip44Root = false;
    obj.push_back(Pair("type", "Loose"));
    obj.push_back(Pair("active", sek.nFlags & EAF_ACTIVE ? "true" : "false"));
    obj.push_back(Pair("receive_on", sek.nFlags & EAF_RECEIVE_ON ? "true" : "false"));
    obj.push_back(Pair("encrypted", sek.nFlags & EAF_IS_CRYPTED ? "true" : "false"));
    obj.push_back(Pair("label", sek.sLabel));
    
    if (reversePlace(&sek.kp.vchFingerprint[0]) == 0)
    {
        obj.push_back(Pair("path", "Root"));
    } else
    {
        mapEKValue_t::iterator mvi = sek.mapValue.find(EKVT_PATH);
        if (mvi != sek.mapValue.end())
        {
            std::string sPath;
            if (0 == PathToString(mvi->second, sPath, 'h'))
                obj.push_back(Pair("path", sPath));
        };
    };
    
    mapEKValue_t::iterator mvi = sek.mapValue.find(EKVT_KEY_TYPE);
    if (mvi != sek.mapValue.end())
    {
        uint8_t type = EKT_MAX_TYPES;
        if (mvi->second.size() == 1)
            type = mvi->second[0];
        
        std::string sType;
        switch (type)
        {
            case EKT_MASTER      : sType = "Master"; break;
            case EKT_BIP44_MASTER:
                sType = "BIP44 Root Key";
                fBip44Root = true;
                break;
            default              : sType = "Unknown"; break;
        };
        obj.push_back(Pair("key_type", sType));
    };
    
    if (idMaster == idKey)
        obj.push_back(Pair("current_master", "true"));
    
    CBitcoinAddress addr;
    mvi = sek.mapValue.find(EKVT_ROOT_ID);
    if (mvi != sek.mapValue.end())
    {
        CKeyID idRoot;
        
        if (GetCKeyID(mvi->second, idRoot))
        {
            addr.Set(idRoot, CChainParams::EXT_KEY_HASH);
            obj.push_back(Pair("root_key_id", addr.ToString()));
        } else
        {
            obj.push_back(Pair("root_key_id", "malformed"));
        };
    };
    
    mvi = sek.mapValue.find(EKVT_CREATED_AT);
    if (mvi != sek.mapValue.end())
    {
        int64_t nCreatedAt;
        GetCompressedInt64(mvi->second, (uint64_t&)nCreatedAt);
        obj.push_back(Pair("created_at", nCreatedAt));
    };
    
    
    addr.Set(idKey, CChainParams::EXT_KEY_HASH);
    obj.push_back(Pair("id", addr.ToString()));
    
    if (nShowKeys > 1
        && pwallet->ExtKeyUnlock(&sek) == 0)
    {
        if (fBip44Root)
            eKey58.SetKey(sek.kp, CChainParams::EXT_SECRET_KEY_BTC);
        else
            eKey58.SetKeyV(sek.kp);
        obj.push_back(Pair("evkey", eKey58.ToString()));
    };
    
    if (nShowKeys > 0)
    {
        if (fBip44Root)
            eKey58.SetKey(sek.kp, CChainParams::EXT_PUBLIC_KEY_BTC);
        else
            eKey58.SetKeyP(sek.kp);
        
        obj.push_back(Pair("epkey", eKey58.ToString()));
    };
    
    obj.push_back(Pair("num_derives", strprintf("%u", sek.nGenerated)));
    obj.push_back(Pair("num_derives_hardened", strprintf("%u", sek.nHGenerated)));
    
    return 0;
};

int KeyInfo(CKeyID &idMaster, CKeyID &idKey, int nShowKeys, UniValue &obj, std::string &sError)
{
    CHDWallet *pwallet = (CHDWallet*) pwalletMain;
    
    CStoredExtKey sek;
    {
        LOCK(pwallet->cs_wallet);
        CHDWalletDB wdb(pwallet->strWalletFile, "r+");
        
        if (!wdb.ReadExtKey(idKey, sek))
        {
            sError = "Key not found in wallet.";
            return 1;
        };
    }
    
    return KeyInfo(idMaster, idKey, sek, nShowKeys, obj, sError);
};

class ListExtCallback : public LoopExtKeyCallback
{
public:
    ListExtCallback(UniValue *arr, int _nShowKeys)
    {
        CHDWallet *pwallet = (CHDWallet*) pwalletMain;
        nItems = 0;
        rvArray = arr;
        nShowKeys = _nShowKeys;
        
        
        if (pwallet && pwallet->pEKMaster)
            idMaster = pwallet->pEKMaster->GetID();
    };

    int ProcessKey(CKeyID &id, CStoredExtKey &sek)
    {
        nItems++;
        UniValue obj(UniValue::VOBJ);
        if (0 != KeyInfo(idMaster, id, sek, nShowKeys, obj, sError))
        {
            obj.push_back(Pair("id", sek.GetIDString58()));
            obj.push_back(Pair("error", sError));
        };
        
        rvArray->push_back(obj);
        return 0;
    };
    
    int ProcessAccount(CKeyID &id, CExtKeyAccount &sea)
    {
        nItems++;
        UniValue obj(UniValue::VOBJ);
        if (0 != AccountInfo(&sea, nShowKeys, false, obj, sError))
        {
            obj.push_back(Pair("id", sea.GetIDString58()));
            obj.push_back(Pair("error", sError));
        };
        
        rvArray->push_back(obj);
        return 0;
    };
    
    std::string sError;
    int nItems;
    int nShowKeys;
    CKeyID idMaster;
    UniValue *rvArray;
};

int ListLooseExtKeys(int nShowKeys, UniValue &ret, size_t &nKeys)
{
    ListExtCallback cbc(&ret, nShowKeys);
    
    if (0 != LoopExtKeysInDB(true, false, cbc))
        return errorN(1, "LoopExtKeys failed.");
    
    nKeys = cbc.nItems;
    
    return 0;
};

int ListAccountExtKeys(int nShowKeys, UniValue &ret, size_t &nKeys)
{
    ListExtCallback cbc(&ret, nShowKeys);
    
    if (0 != LoopExtAccountsInDB(true, cbc))
        return errorN(1, "LoopExtKeys failed.");
    
    nKeys = cbc.nItems;
    
    return 0;
};

int ManageExtKey(CStoredExtKey &sek, std::string &sOptName, std::string &sOptValue, UniValue &result, std::string &sError)
{
    if (sOptName == "label")
    {
        if (sOptValue.length() == 0)
            sek.sLabel = sOptValue;
        
        result.push_back(Pair("set_label", sek.sLabel));
    } else
    if (sOptName == "active")
    {
        if (sOptValue.length() > 0)
        {
            if (part::IsStringBoolPositive(sOptValue))
                sek.nFlags |= EAF_ACTIVE;
            else
                sek.nFlags &= ~EAF_ACTIVE;
        };
        
        result.push_back(Pair("set_active", sek.nFlags & EAF_ACTIVE ? "true" : "false"));
    } else
    if (sOptName == "receive_on")
    {
        if (sOptValue.length() > 0)
        {
            if (part::IsStringBoolPositive(sOptValue))
                sek.nFlags |= EAF_RECEIVE_ON;
            else
                sek.nFlags &= ~EAF_RECEIVE_ON;
        };
        
        result.push_back(Pair("receive_on", sek.nFlags & EAF_RECEIVE_ON ? "true" : "false"));
    } else
    if (sOptName == "look_ahead")
    {
        uint64_t nLookAhead = GetArg("-defaultlookaheadsize", N_DEFAULT_LOOKAHEAD);
        
        if (sOptValue.length() > 0)
        {
            char *pend;
            errno = 0;
            nLookAhead = strtoul(sOptValue.c_str(), &pend, 10);
            if (errno != 0 || !pend || *pend != '\0')
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed: look_ahead invalid number.");
            
            if (nLookAhead < 1 || nLookAhead > 1000)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed: look_ahead number out of range.");
            
            std::vector<uint8_t> v;
            sek.mapValue[EKVT_N_LOOKAHEAD] = SetCompressedInt64(v, nLookAhead);
            result.push_back(Pair("note", "Wallet must be restarted to reload lookahead pool."));
        };
        
        mapEKValue_t::iterator itV = sek.mapValue.find(EKVT_N_LOOKAHEAD);
        if (itV != sek.mapValue.end())
        {
            nLookAhead = GetCompressedInt64(itV->second, nLookAhead);
            result.push_back(Pair("look_ahead", (int)nLookAhead));
        } else
        {
            result.push_back(Pair("look_ahead", "default"));
        };
    } else
    {
        // List all possible
        result.push_back(Pair("label", sek.sLabel));
        result.push_back(Pair("active", sek.nFlags & EAF_ACTIVE ? "true" : "false"));
        result.push_back(Pair("receive_on", sek.nFlags & EAF_RECEIVE_ON ? "true" : "false"));
        
        
        mapEKValue_t::iterator itV = sek.mapValue.find(EKVT_N_LOOKAHEAD);
        if (itV != sek.mapValue.end())
        {
            uint64_t nLookAhead = GetCompressedInt64(itV->second, nLookAhead);
            result.push_back(Pair("look_ahead", (int)nLookAhead));
        } else
        {
            result.push_back(Pair("look_ahead", "default"));
        };
    };
    
    return 0;
};

int ManageExtAccount(CExtKeyAccount &sea, std::string &sOptName, std::string &sOptValue, UniValue &result, std::string &sError)
{
    if (sOptName == "label")
    {
        if (sOptValue.length() > 0)
            sea.sLabel = sOptValue;
        
        result.push_back(Pair("set_label", sea.sLabel));
    } else
    if (sOptName == "active")
    {
        if (sOptValue.length() > 0)
        {
            if (part::IsStringBoolPositive(sOptValue))
                sea.nFlags |= EAF_ACTIVE;
            else
                sea.nFlags &= ~EAF_ACTIVE;
        };
        
        result.push_back(Pair("set_active", sea.nFlags & EAF_ACTIVE ? "true" : "false"));
    } else
    {
        // List all possible
        result.push_back(Pair("label", sea.sLabel));
        result.push_back(Pair("active", sea.nFlags & EAF_ACTIVE ? "true" : "false"));
    };
    
    return 0;
};

static int ExtractExtKeyId(const std::string &sInKey, CKeyID &keyId, CChainParams::Base58Type prefix)
{
    CExtKey58 eKey58;
    CExtKeyPair ekp;
    CBitcoinAddress addr;
    
    if (addr.SetString(sInKey)
        && addr.IsValid(prefix)
        && addr.GetKeyID(keyId, prefix))
    {
        // keyId is set
    } else
    if (eKey58.Set58(sInKey.c_str()) == 0)
    {
        ekp = eKey58.GetKey();
        keyId = ekp.GetID();
    } else
    {
        throw std::runtime_error("Invalid key.");
    };
    return 0;
};

UniValue extkey(const JSONRPCRequest &request)
{
    static const char *help = ""
        "extkey [info|list|account|gen|import|importAccount|setMaster|setDefaultAccount|deriveAccount|options]\n"
        "extkey [\"info\"] [key] [path]\n"
        "extkey list [show_secrets] - default\n"
        "    List loose and account ext keys.\n"
        "extkey account <key/id> [show_secrets]\n"
        "    Display details of account.\n"
        "extkey key <key/id> [show_secrets]\n"
        "    Display details of loose key.\n"
        "extkey gen [passphrase] [num hashes] [seed string]\n"
        "    If no passhrase is specified key will be generated from random data.\n"
        "    Warning: It is recommended to not use the passphrase\n"
        "extkey import <key> [label] [bip44] [save_bip44_key]\n"
        "    Add loose key to wallet.\n"
        "    If bip44 is set import will add the key derived from <key> on the bip44 path.\n"
        "    If save_bip44_key is set import will save the bip44 key to the wallet.\n"
        "extkey importAccount <key> [time_scan_from] [label] \n"
        "    Add account key to wallet.\n"
        "        time_scan_from: N no check, Y-m-d date to start scanning the blockchain for owned txns.\n"
        "extkey setMaster <key/id>\n"
        "    Set a private ext key as current master key.\n"
        "    key can be a extkeyid or full key, but must be in the wallet.\n"
        "extkey setDefaultAccount <id>\n"
        "    Set an account as the default.\n"
        "extkey deriveAccount [label] [path]\n"
        "    Make a new account from the current master key, saves to wallet.\n"
        "extkey options <key> [optionName] [newValue]\n"
        "    Manage keys and accounts\n"
        "\n";
    
    
    // default mode is list unless 1st parameter is a key - then mode is set to info
    
    // path:
    // master keys are hashed with an integer (child_index) to form child keys
    // each child key can spawn more keys
    // payments etc are not send to keys derived from the master keys 
    //  m - master key
    //  m/0 - key0 (1st) key derived from m
    //  m/1/2 key2 (3rd) key derived from key1 derived from m
    
    // hardened keys are keys with (child_index) > 2^31
    // it's not possible to compute the next extended public key in the sequence from a hardened public key (still possible with a hardened private key)
    
    // this maintains privacy, you can give hardened public keys to customers
    // and they will not be able to compute/guess the key you give out to other customers
    // but will still be able to send payments to you on the 2^32 keys derived from the public key you provided
    
    
    // accounts to receive must be non-hardened
    //   - locked wallets must be able to derive new keys as they receive
    
    if (request.fHelp || request.params.size() > 5) // defaults to info, will always take at least 1 parameter
        throw std::runtime_error(help);
    
    CHDWallet *pwallet = GetHDWallet();
    
    EnsureWalletIsUnlocked(pwallet);
    
    std::string mode = "list";
    std::string sInKey = "";
    
    uint32_t nParamOffset = 0;
    if (request.params.size() > 0)
    {
        std::string s = request.params[0].get_str();
        std::string st = " " + s + " "; // Note the spaces
        std::transform(st.begin(), st.end(), st.begin(), ::tolower);
        static const char *pmodes = " info list gen account key import importaccount setmaster setdefaultaccount deriveaccount options ";
        if (strstr(pmodes, st.c_str()) != NULL)
        {
            st.erase(std::remove(st.begin(), st.end(), ' '), st.end());
            mode = st;
            
            nParamOffset = 1;
        } else
        {
            sInKey = s;
            mode = "info";
            nParamOffset = 1;
        };
    };
    
    CBitcoinExtKey bvk;
    CBitcoinExtPubKey bpk;
    
    std::vector<uint8_t> vchVersionIn;
    vchVersionIn.resize(4);
    
    UniValue result(UniValue::VOBJ);
    
    if (mode == "info")
    {
        std::string sMode = "info"; // info lists details of bip32 key, m displays internal key
        
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            };
        };
        
        if (request.params.size() > nParamOffset)
            sMode = request.params[nParamOffset].get_str();
        
        UniValue keyInfo(UniValue::VOBJ);
        std::vector<uint8_t> vchOut;
        
        if (!DecodeBase58(sInKey.c_str(), vchOut))
            throw std::runtime_error("DecodeBase58 failed.");
        if (!VerifyChecksum(vchOut))
            throw std::runtime_error("VerifyChecksum failed.");
        
        size_t keyLen = vchOut.size();
        std::string sError;
        
        if (keyLen != BIP32_KEY_LEN)
            throw std::runtime_error(strprintf("Unknown ext key length '%d'", keyLen));
        
        const CChainParams &otherNet = 
            (Params().NetworkID() == CBaseChainParams::TESTNET || Params().NetworkID() == CBaseChainParams::REGTEST)
            ? Params(CBaseChainParams::MAIN) : Params(CBaseChainParams::TESTNET);
        
        if (memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY)[0], 4) == 0
            || memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY_BTC)[0], 4) == 0)
        {
            if (ExtKeyPathV(sMode, vchOut, keyInfo, sError) != 0)
                throw std::runtime_error(strprintf("ExtKeyPathV failed %s.", sError.c_str()));
        } else
        if (memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY)[0], 4) == 0
            || memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY_BTC)[0], 4) == 0)
        {
            if (ExtKeyPathP(sMode, vchOut, keyInfo, sError) != 0)
                throw std::runtime_error(strprintf("ExtKeyPathP failed %s.", sError.c_str()));
        } else
        if (memcmp(&vchOut[0], &otherNet.Base58Prefix(CChainParams::EXT_SECRET_KEY)[0], 4) == 0
            || memcmp(&vchOut[0], &otherNet.Base58Prefix(CChainParams::EXT_SECRET_KEY_BTC)[0], 4) == 0
            || memcmp(&vchOut[0], &otherNet.Base58Prefix(CChainParams::EXT_PUBLIC_KEY)[0], 4) == 0
            || memcmp(&vchOut[0], &otherNet.Base58Prefix(CChainParams::EXT_PUBLIC_KEY_BTC)[0], 4) == 0)
        {
            throw std::runtime_error(strprintf("Prefix is for %s-net bip32 key.", otherNet.NetworkIDString().c_str()));
        } else
        {
            throw std::runtime_error(strprintf("Unknown prefix '%s'", sInKey.substr(0, 4)));
        };
        
        result.push_back(Pair("key_info", keyInfo));
    } else
    if (mode == "list")
    {
        UniValue ret(UniValue::VARR);
        
        int nListFull = 0; // 0 id only, 1 id+pubkey, 2 id+pubkey+secret
        if (request.params.size() > nParamOffset)
        {
            std::string st = request.params[nParamOffset].get_str();
            if (part::IsStringBoolPositive(st))
                nListFull = 2;
            
            nParamOffset++;
        };
        
        size_t nKeys = 0, nAcc = 0;
        
        {
            LOCK(pwallet->cs_wallet);
            ListLooseExtKeys(nListFull, ret, nKeys);
            ListAccountExtKeys(nListFull, ret, nAcc);
        } // cs_wallet
        
        if (nKeys + nAcc > 0)
            return ret;
        
        result.push_back(Pair("result", "No keys to list."));
    } else
    if (mode == "account"
        || mode == "key")
    {
        CKeyID keyId;
        if (request.params.size() > nParamOffset)
        {
            sInKey = request.params[nParamOffset].get_str();
            nParamOffset++;
            
            ExtractExtKeyId(sInKey, keyId, mode == "account" ? CChainParams::EXT_ACC_HASH : CChainParams::EXT_KEY_HASH);
        } else
        {
            // Display default account
            if (mode == "account")
                keyId = pwallet->idDefaultAccount;
            
            if (keyId.IsNull())
                throw std::runtime_error("Must specify ext key or id.");
        };
        
        int nListFull = 0; // 0 id only, 1 id+pubkey, 2 id+pubkey+secret
        if (request.params.size() > nParamOffset)
        {
            std::string st = request.params[nParamOffset].get_str();
            if (part::IsStringBoolPositive(st))
                nListFull = 2;
            
            nParamOffset++;
        };
        
        std::string sError;
        if (mode == "account")
        {
            if (0 != AccountInfo(keyId, nListFull, true, result, sError))
                throw std::runtime_error("AccountInfo failed: " + sError);
        } else
        {
            CKeyID idMaster;
            if (pwallet->pEKMaster)
                idMaster = pwallet->pEKMaster->GetID();
            else
                LogPrintf("%s: Warning: Master key isn't set!\n", __func__);
            if (0 != KeyInfo(idMaster, keyId, nListFull, result, sError))
                throw std::runtime_error("KeyInfo failed: " + sError);
        };
    } else
    if (mode == "gen")
    {
        // Make a new master key
        // from random or passphrase + int + seed string
        
        CExtKey newKey;
        
        CBitcoinExtKey b58Key;
        
        if (request.params.size() > 1)
        {
            std::string sPassphrase = request.params[1].get_str();
            int32_t nHashes = 100;
            std::string sSeed = "Bitcoin seed";
            
            // Generate from passphrase
            //   allow generator string and nhashes to be specified
            //   To allow importing of bip32 strings from other systems
            //   Match bip32.org: bip32 gen "pass" 50000 "Bitcoin seed"
            
            if (request.params.size() > 2)
            {
                std::stringstream sstr(request.params[2].get_str());
                
                sstr >> nHashes;
                if (!sstr)
                    throw std::runtime_error("Invalid num hashes");
                
                if (nHashes < 1)
                    throw std::runtime_error("Num hashes must be 1 or more.");
            };
            
            if (request.params.size() > 3)
            {
                sSeed = request.params[3].get_str();
            };
            
            if (request.params.size() > 4)
                throw std::runtime_error(help);
            
            pwallet->ExtKeyNew32(newKey, sPassphrase.c_str(), nHashes, sSeed.c_str());
            
            result.push_back(Pair("warning",
                "If the same passphrase is used by another your privacy and coins will be compromised.\n"
                "It is not recommended to use this feature - if you must, pick very unique values for passphrase, num hashes and generator parameters."));
        } else
        {
             pwallet->ExtKeyNew32(newKey);
        };
        
        b58Key.SetKey(newKey);
        
        result.push_back(Pair("result", b58Key.ToString()));
    } else
    if (mode == "import")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            };
        };
        
        CStoredExtKey sek;
        if (request.params.size() > nParamOffset)
        {
            sek.sLabel = request.params[nParamOffset].get_str();
            nParamOffset++;
        };
        
        bool fBip44 = false;
        if (request.params.size() > nParamOffset)
        {
            std::string s = request.params[nParamOffset].get_str();
            if (part::IsStringBoolPositive(s))
                fBip44 = true;
            nParamOffset++;
        };
        
        bool fSaveBip44 = false;
        if (request.params.size() > nParamOffset)
        {
            std::string s = request.params[nParamOffset].get_str();
            if (part::IsStringBoolPositive(s))
                fSaveBip44 = true;
            nParamOffset++;
        };
        
        std::vector<uint8_t> v;
        sek.mapValue[EKVT_CREATED_AT] = SetCompressedInt64(v, GetTime());
        
        CExtKey58 eKey58;
        if (eKey58.Set58(sInKey.c_str()) != 0)
            throw std::runtime_error("Import failed - Invalid key.");
        
        if (fBip44)
        {
            if (!eKey58.IsValid(CChainParams::EXT_SECRET_KEY_BTC))
                throw std::runtime_error("Import failed - BIP44 key must begin with a bitcoin secret key prefix.");
        } else
        {
            if (!eKey58.IsValid(CChainParams::EXT_SECRET_KEY)
                && !eKey58.IsValid(CChainParams::EXT_PUBLIC_KEY_BTC))
                throw std::runtime_error("Import failed - Key must begin with a particl prefix.");
        };
        
        sek.kp = eKey58.GetKey();
        
        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->strWalletFile, "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");
            
            int rv;
            CKeyID idDerived;
            if (0 != (rv = pwallet->ExtKeyImportLoose(&wdb, sek, idDerived, fBip44, fSaveBip44)))
            {
                wdb.TxnAbort();
                throw std::runtime_error(strprintf("ExtKeyImportLoose failed, %s", ExtKeyGetString(rv)));
            };
            
            if (!wdb.TxnCommit())
                throw std::runtime_error("TxnCommit failed.");
            result.push_back(Pair("result", "Success."));
            result.push_back(Pair("key_label", sek.sLabel));
            result.push_back(Pair("note", "Please backup your wallet.")); // TODO: check for child of existing key? 
        } // cs_wallet
    } else
    if (mode == "importaccount")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            };
        };
        
        int64_t nTimeStartScan = 1; // scan from start, 0 means no scan
        if (request.params.size() > nParamOffset)
        {
            std::string sVar = request.params[nParamOffset].get_str();
            nParamOffset++;
            
            if (sVar == "N")
            {
                nTimeStartScan = 0;
            } else
            if (part::IsStrOnlyDigits(sVar))
            {
                // Setting timestamp directly
                errno = 0;
                nTimeStartScan = strtoimax(sVar.c_str(), NULL, 10);
                if (errno != 0)
                    throw std::runtime_error("Import Account failed - Parse time error.");
            } else
            {
                int year, month, day;
                
                if (sscanf(sVar.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
                    throw std::runtime_error("Import Account failed - Parse time error.");
                
                struct tm tmdate;
                tmdate.tm_year = year - 1900;
                tmdate.tm_mon = month - 1;
                tmdate.tm_mday = day;
                time_t t = mktime(&tmdate);
                
                nTimeStartScan = t;
            };
        };
        
        std::string sLabel;
        if (request.params.size() > nParamOffset)
        {
            sLabel = request.params[nParamOffset].get_str();
            nParamOffset++;
        };
        
        CStoredExtKey sek;
        CExtKey58 eKey58;
        if (eKey58.Set58(sInKey.c_str()) == 0)
        {
            sek.kp = eKey58.GetKey();
        } else
        {
            throw std::runtime_error("Import Account failed - Invalid key.");
        };
        
        {
            //LOCK(pwallet->cs_wallet);
            LOCK2(cs_main, pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->strWalletFile, "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");
            
            int rv = pwallet->ExtKeyImportAccount(&wdb, sek, nTimeStartScan, sLabel);
            if (rv == 1)
            {
                wdb.TxnAbort();
                throw std::runtime_error("Import failed - ExtKeyImportAccount failed.");
            } else
            if (rv == 2)
            {
                wdb.TxnAbort();
                throw std::runtime_error("Import failed - account exists.");
            } else
            {
                if (!wdb.TxnCommit())
                    throw std::runtime_error("TxnCommit failed.");
                result.push_back(Pair("result", "Success."));
                
                if (rv == 3)
                    result.push_back(Pair("result", "secret added to existing account."));
                
                result.push_back(Pair("account_label", sLabel));
                result.push_back(Pair("scanned_from", nTimeStartScan));
                result.push_back(Pair("note", "Please backup your wallet.")); // TODO: check for child of existing key?
            };
            
        } // cs_wallet
    } else
    if (mode == "setmaster")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            } else
                throw std::runtime_error("Must specify ext key or id.");
        };
        
        CKeyID idNewMaster;
        ExtractExtKeyId(sInKey, idNewMaster, CChainParams::EXT_KEY_HASH);
        
        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->strWalletFile, "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");
            
            int rv;
            if (0 != (rv = pwallet->ExtKeySetMaster(&wdb, idNewMaster)))
            {
                wdb.TxnAbort();
                throw std::runtime_error(strprintf("ExtKeySetMaster failed, %s.", ExtKeyGetString(rv)));
            };
            if (!wdb.TxnCommit())
                throw std::runtime_error("TxnCommit failed.");
            result.push_back(Pair("result", "Success."));
        } // cs_wallet
        
    } else
    if (mode == "setdefaultaccount")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            } else
                throw std::runtime_error("Must specify ext key or id.");
        };
        
        CKeyID idNewDefault;
        CKeyID idOldDefault = pwallet->idDefaultAccount;
        CBitcoinAddress addr;
        
        CExtKeyAccount *sea = new CExtKeyAccount();
        
        if (addr.SetString(sInKey)
            && addr.IsValid(CChainParams::EXT_ACC_HASH)
            && addr.GetKeyID(idNewDefault, CChainParams::EXT_ACC_HASH))
        {
            // idNewDefault is set
        };
        
        int rv;
        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->strWalletFile, "r+");
            
            if (!wdb.TxnBegin())
            {
                delete sea;
                throw std::runtime_error("TxnBegin failed.");
            };
            if (0 != (rv = pwallet->ExtKeySetDefaultAccount(&wdb, idNewDefault)))
            {
                delete sea;
                wdb.TxnAbort();
                throw std::runtime_error(strprintf("ExtKeySetDefaultAccount failed, %s.", ExtKeyGetString(rv)));
            };
            if (!wdb.TxnCommit())
            {
                delete sea;
                pwallet->idDefaultAccount = idOldDefault;
                throw std::runtime_error("TxnCommit failed.");
            };
            
            result.push_back(Pair("result", "Success."));
        } // cs_wallet
        
    } else
    if (mode == "deriveaccount")
    {
        std::string sLabel, sPath;
        if (request.params.size() > nParamOffset)
        {
            sLabel = request.params[nParamOffset].get_str();
            nParamOffset++;
        };
        
        if (request.params.size() > nParamOffset)
        {
            sPath = request.params[nParamOffset].get_str();
            nParamOffset++;
        };
        
        for (; nParamOffset < request.params.size(); nParamOffset++)
        {
            std::string strParam = request.params[nParamOffset].get_str();
            std::transform(strParam.begin(), strParam.end(), strParam.begin(), ::tolower);
            
            throw std::runtime_error(strprintf("Unknown parameter '%s'", strParam.c_str()));
        };
        
        CExtKeyAccount *sea = new CExtKeyAccount();
        
        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->strWalletFile, "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");
            
            int rv;
            if ((rv = pwallet->ExtKeyDeriveNewAccount(&wdb, sea, sLabel, sPath)) != 0)
            {
                wdb.TxnAbort();
                result.push_back(Pair("result", "Failed."));
                result.push_back(Pair("reason", ExtKeyGetString(rv)));
            } else
            {
                if (!wdb.TxnCommit())
                    throw std::runtime_error("TxnCommit failed.");
                
                result.push_back(Pair("result", "Success."));
                result.push_back(Pair("account", sea->GetIDString58()));
                CStoredExtKey *sekAccount = sea->ChainAccount();
                if (sekAccount)
                {
                    CExtKey58 eKey58;
                    eKey58.SetKeyP(sekAccount->kp);
                    result.push_back(Pair("public key", eKey58.ToString()));
                };
                
                if (sLabel != "")
                    result.push_back(Pair("label", sLabel));
            };
        } // cs_wallet
    } else
    if (mode == "options")
    {
        std::string sOptName, sOptValue, sError;
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            } else
                throw std::runtime_error("Must specify ext key or id.");
        };
        if (request.params.size() > nParamOffset)
        {
            sOptName = request.params[nParamOffset].get_str();
            nParamOffset++;
        };
        if (request.params.size() > nParamOffset)
        {
            sOptValue = request.params[nParamOffset].get_str();
            nParamOffset++;
        };
        
        CBitcoinAddress addr;
        
        CKeyID id;
        if (!addr.SetString(sInKey))
            throw std::runtime_error("Invalid key or account id.");
        
        bool fAccount = false;
        bool fKey = false;
        if (addr.IsValid(CChainParams::EXT_KEY_HASH)
            && addr.GetKeyID(id, CChainParams::EXT_KEY_HASH))
        {
            // id is set
            fKey = true;
        } else
        if (addr.IsValid(CChainParams::EXT_ACC_HASH)
            && addr.GetKeyID(id, CChainParams::EXT_ACC_HASH))
        {
            // id is set
            fAccount = true;
        } else
        if (addr.IsValid(CChainParams::EXT_PUBLIC_KEY))
        {
            CExtKeyPair ek = boost::get<CExtKeyPair>(addr.Get());
            
            id = ek.GetID();
            
            ExtKeyAccountMap::iterator it = pwallet->mapExtAccounts.find(id);
            if (it != pwallet->mapExtAccounts.end())
                fAccount = true;
            else
                fKey = true;
        } else
        {
            throw std::runtime_error("Invalid key or account id.");
        };
        
        CStoredExtKey sek;
        CExtKeyAccount sea;
        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->strWalletFile, "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");
            
            if (fKey)
            {
                // Try key in memory first
                CStoredExtKey *pSek;
                ExtKeyMap::iterator it = pwallet->mapExtKeys.find(id);
                
                if (it != pwallet->mapExtKeys.end())
                {
                    pSek = it->second;
                } else
                if (wdb.ReadExtKey(id, sek))
                {
                    pSek = &sek;
                } else
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Key not in wallet.");
                };
                
                if (0 != ManageExtKey(*pSek, sOptName, sOptValue, result, sError))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Error: " + sError);
                };
                
                if (sOptValue.length() > 0
                    && !wdb.WriteExtKey(id, *pSek))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("WriteExtKey failed.");
                };
            };
            
            if (fAccount)
            {
                CExtKeyAccount *pSea;
                ExtKeyAccountMap::iterator it = pwallet->mapExtAccounts.find(id);
                if (it != pwallet->mapExtAccounts.end())
                {
                    pSea = it->second;
                } else
                if (wdb.ReadExtAccount(id, sea))
                {
                    pSea = &sea;
                } else
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Account not in wallet.");
                };
                
                if (0 != ManageExtAccount(*pSea, sOptName, sOptValue, result, sError))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Error: " + sError);
                };
                
                if (sOptValue.length() > 0
                    && !wdb.WriteExtAccount(id, *pSea))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Write failed.");
                };
            };
            
            if (sOptValue.length() == 0)
            {
                wdb.TxnAbort();
            } else
            {
                if (!wdb.TxnCommit())
                    throw std::runtime_error("TxnCommit failed.");
                result.push_back(Pair("result", "Success."));
            };
        } // cs_wallet
        
    } else
    {
        throw std::runtime_error(help);
    };
    
    return result;
};

UniValue extkeyimportinternal(const JSONRPCRequest &request, bool fGenesisChain)
{
    CHDWallet *pwallet = GetHDWallet();
    
    EnsureWalletIsUnlocked(pwallet);
    
    if (request.params.size() < 1)
        throw std::runtime_error("Please specify a private extkey or mnemonic phrase.");
    
    std::string sMnemonic = request.params[0].get_str();
    bool fSaveBip44Root = false;
    std::string sLblMaster = "Master Key";
    std::string sLblAccount = "Default Account";
    std::string sPassphrase = "";
    std::string sError;
    
    if (request.params.size() > 1)
        sPassphrase = request.params[1].get_str();
    
    if (request.params.size() > 2)
    {
        std::string s = request.params[2].get_str();
        
        if (!part::GetStringBool(s, fSaveBip44Root))
            throw std::runtime_error(strprintf("Unknown argument for save_bip44_root: %s.", s.c_str()));
    };
    
    if (request.params.size() > 3)
        sLblMaster = request.params[3].get_str();
    if (request.params.size() > 4)
        sLblAccount = request.params[4].get_str();
    if (request.params.size() > 5)
        throw std::runtime_error(strprintf("Unknown parameter '%s'", request.params[5].get_str()));
    
    LogPrintf("Importing master key and account with labels '%s', '%s'.\n", sLblMaster.c_str(), sLblAccount.c_str());
    
    CExtKey58 eKey58;
    CExtKeyPair ekp;
    if (eKey58.Set58(sMnemonic.c_str()) == 0)
    {
        if (!eKey58.IsValid(CChainParams::EXT_SECRET_KEY)
            && !eKey58.IsValid(CChainParams::EXT_SECRET_KEY_BTC))
            throw std::runtime_error("Please specify a private extkey or mnemonic phrase.");
        
        // Key was provided directly
        ekp = eKey58.GetKey();
    } else
    {
        std::vector<uint8_t> vSeed, vEntropy;
        
        // First check the mnemonic is valid
        if (0 != MnemonicDecode(-1, sMnemonic, vEntropy, sError))
            throw std::runtime_error(strprintf("MnemonicDecode failed: %s", sError.c_str()));
        
        if (0 != MnemonicToSeed(sMnemonic, sPassphrase, vSeed))
            throw std::runtime_error("MnemonicToSeed failed.");
        
        ekp.SetMaster(&vSeed[0], vSeed.size());
    };
    
    CStoredExtKey sek;
    sek.sLabel = sLblMaster;
    
    std::vector<uint8_t> v;
    sek.mapValue[EKVT_CREATED_AT] = SetCompressedInt64(v, GetTime());
    sek.kp = ekp;
    
    UniValue result(UniValue::VOBJ);
    
    int rv;
    bool fBip44 = true;
    CKeyID idDerived;
    CExtKeyAccount *sea;
    
    {
        LOCK(pwallet->cs_wallet);
        CHDWalletDB wdb(pwallet->strWalletFile, "r+");
        if (!wdb.TxnBegin())
            throw std::runtime_error("TxnBegin failed.");
        
        if (0 != (rv = pwallet->ExtKeyImportLoose(&wdb, sek, idDerived, fBip44, fSaveBip44Root)))
        {
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeyImportLoose failed, %s", ExtKeyGetString(rv)));
        };
        
        if (0 != (rv = pwallet->ExtKeySetMaster(&wdb, idDerived)))
        {
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeySetMaster failed, %s.", ExtKeyGetString(rv)));
        };
        
        sea = new CExtKeyAccount();
        if (0 != (rv = pwallet->ExtKeyDeriveNewAccount(&wdb, sea, sLblAccount)))
        {
            pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeyDeriveNewAccount failed, %s.", ExtKeyGetString(rv)));
        };
        
        CKeyID idNewDefaultAccount = sea->GetID();
        CKeyID idOldDefault = pwallet->idDefaultAccount;
        
        if (0 != (rv = pwallet->ExtKeySetDefaultAccount(&wdb, idNewDefaultAccount)))
        {
            pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeySetDefaultAccount failed, %s.", ExtKeyGetString(rv)));
        };
        
        if (fGenesisChain)
        {
            std::string genesisChainLabel = "Genesis Import";
            uint32_t genesisChainNo = 444444;
            CStoredExtKey *sekGenesisChain = new CStoredExtKey();
            
            if (0 != (rv = pwallet->NewExtKeyFromAccount(&wdb, idNewDefaultAccount,
                genesisChainLabel, sekGenesisChain, NULL, &genesisChainNo)))
            {
                delete sekGenesisChain;
                pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
                wdb.TxnAbort();
                throw std::runtime_error(strprintf("NewExtKeyFromAccount failed, %s.", ExtKeyGetString(rv)));
            };
        };
        
        if (!wdb.TxnCommit())
        {
            pwallet->idDefaultAccount = idOldDefault;
            pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
            throw std::runtime_error("TxnCommit failed.");
        };
    } // cs_wallet

    if (0 != pwallet->ScanChainFromTime(1))
        throw std::runtime_error("ScanChainFromTime failed.");
    
    CBitcoinAddress addr;
    addr.Set(idDerived, CChainParams::EXT_KEY_HASH);
    result.push_back(Pair("result", "Success."));
    result.push_back(Pair("master_id", addr.ToString()));
    result.push_back(Pair("master_label", sek.sLabel));
    
    result.push_back(Pair("account_id", sea->GetIDString58()));
    result.push_back(Pair("account_label", sea->sLabel));
    
    result.push_back(Pair("note", "Please backup your wallet."));
    
    return result;
}

UniValue extkeyimportmaster(const JSONRPCRequest &request)
{
    static const char *help = ""
        "extkeyimportmaster <mnemonic/key> [passphrase] [save_bip44_root] [master_label] [account_label]\n"
        "Import master key from bip44 mnemonic root key and derive default account.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "       if mnemonic is blank, defaults to '-stdin'.\n"
        "   passphrase:         passphrase when importing mnemonic - default blank.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "   save_bip44_root:    Save bip44 root key to wallet - default false.\n"
        "   master_label:       Label for master key - default 'Master Key'.\n"
        "   account_label:      Label for account - default 'Default Account'.\n"
        "Examples:\n"
        "   extkeyimportmaster -stdin -stdin false label_master label_account\n"
        "\n";
    
    // Doesn't generate key, require users to run mnemonic new, more likely they'll save the phrase
    
    if (request.fHelp)
        throw std::runtime_error(help);
    
    return extkeyimportinternal(request, false);
};

UniValue extkeygenesisimport(const JSONRPCRequest &request)
{
    static const char *help = ""
        "extkeygenesisimport <mnemonic/key> [passphrase] [save_bip44_root] [master_label] [account_label]\n"
        "Import master key from bip44 mnemonic root key and derive default account.\n"
        "Derives an extra chain from path 444444 to receive imported coin.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "       if mnemonic is blank, defaults to '-stdin'.\n"
        "   passphrase:         passphrase when importing mnemonic - default blank.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "   save_bip44_root:    Save bip44 root key to wallet - default false.\n"
        "   master_label:       Label for master key - default 'Master Key'.\n"
        "   account_label:      Label for account - default 'Default Account'.\n"
        "Examples:\n"
        "   extkeygenesisimport -stdin -stdin false label_master label_account\n"
        "\n";
    
    if (request.fHelp)
        throw std::runtime_error(help);
    
    return extkeyimportinternal(request, true);
}


UniValue keyinfo(const JSONRPCRequest &request)
{
    static const char *help = ""
        "keyinfo <key> [show_secret]\n"
        "Return public key.\n"
        "\n";
    
    if (request.fHelp) // defaults to info, will always take at least 1 parameter
        throw std::runtime_error(help);
    
    CHDWallet *pwallet = GetHDWallet();
    
    if (request.params.size() < 1)
        throw std::runtime_error("Please specify a key.");
    
    // TODO: show public keys with unlocked wallet?
    EnsureWalletIsUnlocked(pwallet);
    
    
    std::string sKey = request.params[0].get_str();
    
    UniValue result(UniValue::VOBJ);
    
    
    CExtKey58 eKey58;
    CExtKeyPair ekp;
    if (eKey58.Set58(sKey.c_str()) == 0)
    {
        // Key was provided directly
        ekp = eKey58.GetKey();
        result.push_back(Pair("key_type", "extaddress"));
        result.push_back(Pair("mode", ekp.IsValidV() ? "private" : "public"));
        
        CKeyID id = ekp.GetID();
        
        result.push_back(Pair("owned", pwallet->HaveExtKey(id) ? "true" : "false"));
        
        std::string sError;
        
        std::vector<uint8_t> vchOut;
        
        if (!DecodeBase58(sKey.c_str(), vchOut))
            throw std::runtime_error("DecodeBase58 failed.");
        if (!VerifyChecksum(vchOut))
            throw std::runtime_error("VerifyChecksum failed.");
        
        if (ekp.IsValidV())
        {
            if (0 != ExtractBip32InfoV(vchOut, result, sError))
                throw std::runtime_error(strprintf("ExtractBip32InfoV failed %s.", sError.c_str()));
        } else
        {
            if (0 != ExtractBip32InfoP(vchOut, result, sError))
                throw std::runtime_error(strprintf("ExtractBip32InfoP failed %s.", sError.c_str()));
        };
        
        return result;
    }
    
    CBitcoinAddress addr;
    if (addr.SetString(sKey))
    {
        result.push_back(Pair("key_type", "address"));
        
        CKeyID id;
        CPubKey pk;
        if (!addr.GetKeyID(id))
            throw std::runtime_error("GetKeyID failed.");
        
        
        if (!pwallet->GetPubKey(id, pk))
        {
            result.push_back(Pair("result", "Address not in wallet."));
            return result;
        };
        
        result.push_back(Pair("public_key", HexStr(pk.begin(), pk.end())));
        
        
        result.push_back(Pair("result", "Success."));
        return result;
    }
    
    throw std::runtime_error("Unknown keytype.");
};

UniValue extkeyaltversion(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "extkeyaltversion <ext_key>\n"
            "Returns the provided ext_key encoded with alternate version bytes.\n"
            "If the provided ext_key has a Bitcoin prefix the output will be encoded with a Particl prefix.\n"
            "If the provided ext_key has a Particl prefix the output will be encoded with a Bitcoin prefix.");
    
    std::string sKeyIn = request.params[0].get_str();
    std::string sKeyOut;
    
    CExtKey58 eKey58;
    CExtKeyPair ekp;
    if (eKey58.Set58(sKeyIn.c_str()) != 0)
        throw std::runtime_error("Invalid input key.");
    
    // TODO: handle testnet keys on main etc
    if (eKey58.IsValid(CChainParams::EXT_SECRET_KEY_BTC))
        return eKey58.ToStringVersion(CChainParams::EXT_SECRET_KEY);
    if (eKey58.IsValid(CChainParams::EXT_SECRET_KEY))
        return eKey58.ToStringVersion(CChainParams::EXT_SECRET_KEY_BTC);
    
    if (eKey58.IsValid(CChainParams::EXT_PUBLIC_KEY_BTC))
        return eKey58.ToStringVersion(CChainParams::EXT_PUBLIC_KEY);
    if (eKey58.IsValid(CChainParams::EXT_PUBLIC_KEY))
        return eKey58.ToStringVersion(CChainParams::EXT_PUBLIC_KEY_BTC);
    
    throw std::runtime_error("Unknown input key version.");
}


UniValue getnewextaddress(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "getnewextaddress [label] [childNo]\n"
            "Returns a new Particl ext address for receiving payments.\n"
            "label   (string, optional), if specified the key is added to the address book.\n"
            "childNo (int, optional), if specified, the account derive counter is not updated.");
    
    CHDWallet *pwallet = GetHDWallet();
    EnsureWalletIsUnlocked(pwallet);
    
    uint32_t nChild = 0;
    uint32_t *pChild = NULL;
    std::string strLabel;
    const char *pLabel = NULL;
    if (request.params.size() > 0)
    {
        strLabel = request.params[0].get_str();
        if (strLabel.size() > 0)
            pLabel = strLabel.c_str();
    };
    
    if (request.params.size() > 1)
    {
        nChild = request.params[1].get_int();
        pChild = &nChild;
    };

    CStoredExtKey *sek = new CStoredExtKey();
    if (0 != pwallet->NewExtKeyFromAccount(strLabel, sek, pLabel, pChild))
    {
        delete sek;
        throw std::runtime_error("NewExtKeyFromAccount failed.");
    };

    // CBitcoinAddress displays public key only
    return CBitcoinAddress(sek->kp).ToString();
}

UniValue getnewstealthaddress(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "getnewstealthaddress [label] [num_prefix_bits] [prefix_num]\n"
            "Returns a new Particl stealth address for receiving payments."
            "If num_prefix_bits is specified and > 0, the stealth address is created with a prefix.\n"
            "If prefix_num is not specified the prefix will be selected deterministically.\n"
            "prefix_num can be specified in base2, 10 or 16, for base 2 prefix_str must begin with 0b, 0x for base16.\n"
            "A 32bit integer will be created from prefix_num and the least significant num_prefix_bits will become the prefix.\n"
            "A stealth address created without a prefix will scan all incoming stealth transactions, irrespective of transaction prefixes.\n"
            "Stealth addresses with prefixes will scan only incoming stealth transactions with a matching prefix.\n"
            "Examples:\n"
            "   getnewstealthaddress \"lblTestSxAddrPrefix\" 3 \"0b101\" \n"
            + HelpRequiringPassphrase());
    
    CHDWallet *pwallet = GetHDWallet();
    EnsureWalletIsUnlocked(pwallet);
    
    std::string sLabel;
    if (request.params.size() > 0)
        sLabel = request.params[0].get_str();
    
    uint32_t num_prefix_bits = 0;
    if (request.params.size() > 1)
    {
        std::string sTemp = request.params[1].get_str();
        char *pend;
        errno = 0;
        num_prefix_bits = strtoul(sTemp.c_str(), &pend, 10);
        if (errno != 0 || !pend || *pend != '\0')
            throw std::runtime_error("Failed: num_prefix_bits invalid number.");
    };
    
    if (num_prefix_bits > 32)
        throw std::runtime_error("Failed: num_prefix_bits must be <= 32.");
    
    std::string sPrefix_num;
    if (request.params.size() > 2)
        sPrefix_num = request.params[2].get_str();
    
    CEKAStealthKey akStealth;
    std::string sError;

    if (0 != pwallet->NewStealthKeyFromAccount(sLabel, akStealth, num_prefix_bits, sPrefix_num.empty() ? NULL : sPrefix_num.c_str()))
        throw std::runtime_error("NewStealthKeyFromAccount failed.");
    
    CStealthAddress sxAddr;
    akStealth.SetSxAddr(sxAddr);
    
    return sxAddr.ToString();
}

UniValue importstealthaddress(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw std::runtime_error(
            "importstealthaddress <scan_secret> <spend_secret> [label] [num_prefix_bits] [prefix_num]\n"
            "Import an owned stealth addresses.\n"
            "If num_prefix_bits is specified and > 0, the stealth address is created with a prefix.\n"
            "If prefix_num is not specified the prefix will be selected deterministically.\n"
            "prefix_num can be specified in base2, 10 or 16, for base 2 prefix_str must begin with 0b, 0x for base16.\n"
            "A 32bit integer will be created from prefix_num and the least significant num_prefix_bits will become the prefix.\n"
            "A stealth address created without a prefix will scan all incoming stealth transactions, irrespective of transaction prefixes.\n"
            "Stealth addresses with prefixes will scan only incoming stealth transactions with a matching prefix.\n"
            "Examples:\n"
            "   getnewstealthaddress \"lblTestSxAddrPrefix\" 3 \"0b101\" \n"
            + HelpRequiringPassphrase());
    
    CHDWallet *pwallet = GetHDWallet();
    EnsureWalletIsUnlocked(pwallet);

    std::string sScanSecret  = request.params[0].get_str();
    std::string sSpendSecret = request.params[1].get_str();
    std::string sLabel;

    if (request.params.size() > 2)
        sLabel = request.params[2].get_str();
    
    uint32_t num_prefix_bits = 0;
    if (request.params.size() > 3)
    {
        std::string sTemp = request.params[3].get_str();
        char *pend;
        errno = 0;
        num_prefix_bits = strtoul(sTemp.c_str(), &pend, 10);
        if (errno != 0 || !pend || *pend != '\0')
            throw std::runtime_error("Failed: num_prefix_bits invalid number.");
    };
    
    if (num_prefix_bits > 32)
        throw std::runtime_error("Failed: num_prefix_bits must be <= 32.");
    
    uint32_t nPrefix = 0;
    std::string sPrefix_num;
    if (request.params.size() > 4)
    {
        sPrefix_num = request.params[4].get_str();
        if (!ExtractStealthPrefix(sPrefix_num.c_str(), nPrefix))
            throw std::runtime_error("Failed: Could not convert prefix to number.");
    };

    std::vector<uint8_t> vchScanSecret;
    std::vector<uint8_t> vchSpendSecret;
    CBitcoinSecret wifScanSecret, wifSpendSecret;
    CKey skScan, skSpend;
    if (IsHex(sScanSecret))
    {
        vchScanSecret = ParseHex(sScanSecret);
    } else
    if (wifScanSecret.SetString(sScanSecret))
    {
        skScan = wifScanSecret.GetKey();
    } else
    {
        if (!DecodeBase58(sScanSecret, vchScanSecret))
            throw std::runtime_error("Could not decode scan secret as wif, hex or base58.");
    };
    if (vchScanSecret.size() > 0)
    {
        if (vchScanSecret.size() != 32)
            throw std::runtime_error("Scan secret is not 32 bytes.");
        skScan.Set(&vchScanSecret[0], true);
    };

    if (IsHex(sSpendSecret))
    {
        vchSpendSecret = ParseHex(sSpendSecret);
    } else
    if (wifSpendSecret.SetString(sSpendSecret))
    {
        skSpend = wifSpendSecret.GetKey();
    } else
    {
        if (!DecodeBase58(sSpendSecret, vchSpendSecret))
            throw std::runtime_error("Could not decode spend secret as hex or base58.");
    };
    if (vchSpendSecret.size() > 0)
    {
        if (vchSpendSecret.size() != 32)
            throw std::runtime_error("Spend secret is not 32 bytes.");
        skSpend.Set(&vchSpendSecret[0], true);
    };

    CStealthAddress sxAddr;
    sxAddr.label = sLabel;
    sxAddr.scan_secret = skScan;
    sxAddr.spend_secret_id = skSpend.GetPubKey().GetID();
    
    sxAddr.prefix.number_bits = num_prefix_bits;
    if (sxAddr.prefix.number_bits > 0)
    {
        if (sPrefix_num.empty())
        {
            // if pPrefix is null, set nPrefix from the hash of kSpend
            uint8_t tmp32[32];
            CSHA256().Write(skSpend.begin(), 32).Finalize(tmp32);
            memcpy(&nPrefix, tmp32, 4);
        };
        
        uint32_t nMask = SetStealthMask(num_prefix_bits);
        nPrefix = nPrefix & nMask;
        sxAddr.prefix.bitfield = nPrefix;
    };
    
    if (0 != SecretToPublicKey(sxAddr.scan_secret, sxAddr.scan_pubkey))
        throw std::runtime_error("Could not get scan public key.");
    if (0 != SecretToPublicKey(skSpend, sxAddr.spend_pubkey))
        throw std::runtime_error("Could not get spend public key.");

    UniValue result(UniValue::VOBJ);
    bool fFound = false;
    // Find if address already exists, can update 
    std::set<CStealthAddress>::iterator it;
    for (it = pwallet->stealthAddresses.begin(); it != pwallet->stealthAddresses.end(); ++it)
    {
        CStealthAddress &sxAddrIt = const_cast<CStealthAddress&>(*it);
        if (sxAddrIt.scan_pubkey == sxAddr.scan_pubkey
            && sxAddrIt.spend_pubkey == sxAddr.spend_pubkey)
        {
            CKeyID sid = sxAddrIt.GetSpendKeyID();
            
            if (!pwallet->HaveKey(sid))
            {
                CPubKey pk = skSpend.GetPubKey();
                if (!pwallet->AddKeyPubKey(skSpend, pk))
                    throw std::runtime_error("Import failed - AddKeyPubKey failed.");
                fFound = true; // update stealth address with secret
                break;
            };

            throw std::runtime_error("Import failed - stealth address exists.");
        };
    };
    
    {
        LOCK(pwallet->cs_wallet);
        if (pwallet->HaveStealthAddress(sxAddr)) // check for extkeys, no update possible
            throw std::runtime_error("Import failed - stealth address exists.");
        
        pwallet->SetAddressBook(sxAddr, sLabel, "");
    }
    
    if (fFound)
    {
        result.push_back(Pair("result", "Success, updated " + sxAddr.Encoded()));
    } else
    {
        if (!pwallet->ImportStealthAddress(sxAddr, skSpend))
            throw std::runtime_error("Could not save to wallet.");
        result.push_back(Pair("result", "Success"));
        result.push_back(Pair("stealth_address", sxAddr.Encoded()));
    };

    return result;
}

UniValue liststealthaddresses(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "liststealthaddresses [show_secrets=0]\n"
            "List owned stealth addresses.");

    bool fShowSecrets = false;
    
    CHDWallet *pwallet = GetHDWallet();
    
    if (request.params.size() > 0)
    {
        std::string str = request.params[0].get_str();

        if (part::IsStringBoolNegative(str))
            fShowSecrets = false;
        else
            fShowSecrets = true;
    };
    
    if (fShowSecrets)
        EnsureWalletIsUnlocked();

    UniValue result(UniValue::VARR);

    ExtKeyAccountMap::const_iterator mi;
    for (mi = pwallet->mapExtAccounts.begin(); mi != pwallet->mapExtAccounts.end(); ++mi)
    {
        CExtKeyAccount *ea = mi->second;

        if (ea->mapStealthKeys.size() < 1)
            continue;
        
        UniValue rAcc(UniValue::VOBJ);
        UniValue arrayKeys(UniValue::VARR);
        
        rAcc.push_back(Pair("Account", ea->sLabel));

        AccStealthKeyMap::iterator it;
        for (it = ea->mapStealthKeys.begin(); it != ea->mapStealthKeys.end(); ++it)
        {
            const CEKAStealthKey &aks = it->second;
            
            UniValue objA(UniValue::VOBJ);
            objA.push_back(Pair("Label", aks.sLabel));
            objA.push_back(Pair("Address", aks.ToStealthAddress()));
            
            if (fShowSecrets)
            {
                objA.push_back(Pair("Scan Secret", HexStr(aks.skScan.begin(), aks.skScan.end())));
                std::string sSpend;
                CStoredExtKey *sekAccount = ea->ChainAccount();
                if (sekAccount && !sekAccount->fLocked)
                {
                    CKey skSpend;
                    if (ea->GetKey(aks.akSpend, skSpend))
                        sSpend = HexStr(skSpend.begin(), skSpend.end());
                    else
                        sSpend = "Extract failed.";
                } else
                {
                    sSpend = "Account Locked.";
                };
                objA.push_back(Pair("Spend Secret", sSpend));
            };
            
            arrayKeys.push_back(objA);
        };
        
        if (arrayKeys.size() > 0)
        {
            rAcc.push_back(Pair("Stealth Addresses", arrayKeys));
            result.push_back(rAcc);
        };
    };
    
    
    if (pwallet->stealthAddresses.size() > 0)
    {
        UniValue rAcc(UniValue::VOBJ);
        UniValue arrayKeys(UniValue::VARR);
        
        rAcc.push_back(Pair("Account", "Loose Keys"));
        
        std::set<CStealthAddress>::iterator it;
        for (it = pwallet->stealthAddresses.begin(); it != pwallet->stealthAddresses.end(); ++it)
        {
            UniValue objA(UniValue::VOBJ);
            objA.push_back(Pair("Label", it->label));
            objA.push_back(Pair("Address", it->Encoded()));
            
            if (fShowSecrets)
            {
                objA.push_back(Pair("Scan Secret", HexStr(it->scan_secret.begin(), it->scan_secret.end())));
                
                CKeyID sid = it->GetSpendKeyID();
                CKey skSpend;
                if (!pwallet->GetKey(sid, skSpend))
                    throw std::runtime_error("Unknown spend key!");
                
                objA.push_back(Pair("Spend Secret", HexStr(skSpend.begin(), skSpend.end())));
            };
            
            arrayKeys.push_back(objA);
        };
        
        if (arrayKeys.size() > 0)
        {
            rAcc.push_back(Pair("Stealth Addresses", arrayKeys));
            result.push_back(rAcc);
        };
    };
    
    return result;
}


UniValue scanchain(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "scanchain [fromHeight]\n"
            "Scan blockchain for owned transactions.");
    
    CHDWallet *pwallet = GetHDWallet();
    //EnsureWalletIsUnlocked(pwallet);
    
    UniValue result(UniValue::VOBJ);
    int32_t nFromHeight = 0;
    
    if (request.params.size() > 0)
        nFromHeight = request.params[0].get_int();
    
    
    pwallet->ScanChainFromHeight(nFromHeight);
    
    result.push_back(Pair("result", "Scan complete."));
    
    return result;
}

UniValue reservebalance(const JSONRPCRequest &request)
{
    // Reserve balance from being staked for network protection
    
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "reservebalance <reserve> [amount]\n"
            "<reserve> is true or false to turn balance reserve on or off.\n"
            "[amount] is a real and rounded to cent.\n"
            "Set reserve amount not participating in network protection.\n"
            "If no parameters provided current setting is printed.\n"
            "Wallet must be unlocked to modify.\n");
    
    CHDWallet *pwallet = GetHDWallet();
    
    if (request.params.size() > 0)
    {
        EnsureWalletIsUnlocked(pwallet);
        
        bool fReserve = request.params[0].get_bool();
        if (fReserve)
        {
            if (request.params.size() == 1)
                throw std::runtime_error("must provide amount to reserve balance.\n");
            int64_t nAmount = AmountFromValue(request.params[1]);
            nAmount = (nAmount / CENT) * CENT;  // round to cent
            if (nAmount < 0)
                throw std::runtime_error("amount cannot be negative.\n");
            pwallet->SetReserveBalance(nAmount);
        } else
        {
            if (request.params.size() > 1)
                throw std::runtime_error("cannot specify amount to turn off reserve.\n");
            pwallet->SetReserveBalance(0);
        };
        WakeThreadStakeMiner();
    };

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("reserve", (pwallet->nReserveBalance > 0)));
    result.push_back(Pair("amount", ValueFromAmount(pwallet->nReserveBalance)));
    return result;
}

UniValue deriverangekeys(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 ||request.params.size() > 6)
        throw std::runtime_error(
            "deriverangekeys <start> [end] [key/id] [hardened] [save] [add_to_addressbook]\n"
            "<start> start from key.\n"
            "[end] stop deriving after key, default set to derive one key.\n"
            "[key/id] account to derive from, default external chain of current account.\n"
            "[hardened] derive hardened keys, default false.\n"
            "[save] save derived keys to the wallet, default false.\n"
            "[add_to_addressbook] add derived keys to address book, only applies when saving keys, default false.\n"
            "Derive keys from the specified chain.\n"
            "Wallet must be unlocked if save or hardened options are set.\n");
    
    CHDWallet *pwallet = GetHDWallet();
    
    // TODO: manage nGenerated, nHGenerated properly
    
    int nStart = request.params[0].get_int();
    int nEnd = nStart;
    
    if (request.params.size() > 1)
        nEnd = request.params[1].get_int();
    
    if (nEnd < nStart)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "end can not be before start.");
    
    if (nStart < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "start can not be negative.");
    
    if (nEnd < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "end can not be positive.");
    
    std::string sInKey;
    if (request.params.size() > 2)
        sInKey = request.params[2].get_str();
    
    bool fHardened = false;
    if (request.params.size() > 3)
    {
        std::string s = request.params[3].get_str();
        
        if (!part::GetStringBool(s, fHardened))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown argument for hardened: %s.", s.c_str()));
    };
    
    bool fSave = false;
    if (request.params.size() > 4)
    {
        std::string s = request.params[4].get_str();
        
        if (!part::GetStringBool(s, fSave))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown argument for save: %s.", s.c_str()));
    };
    
    bool fAddToAddressBook = false;
    if (request.params.size() > 5)
    {
        std::string s = request.params[5].get_str();
        
        if (!part::GetStringBool(s, fAddToAddressBook))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Unknown argument for add_to_addressbook: %s."), s.c_str()));
    };
    
    if (!fSave && fAddToAddressBook)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("add_to_addressbook can't be set without save"));
    
    if (fSave || fHardened)
        EnsureWalletIsUnlocked(pwallet);
    
    UniValue result(UniValue::VARR);
    
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        
        CStoredExtKey *sek = NULL;
        CExtKeyAccount *sea = NULL;
        uint32_t nChain = 0;
        if (sInKey.length() == 0)
        {
            if (pwallet->idDefaultAccount.IsNull())
                throw JSONRPCError(RPC_WALLET_ERROR, _("No default account set."));
            
            ExtKeyAccountMap::iterator mi = pwallet->mapExtAccounts.find(pwallet->idDefaultAccount);
            if (mi == pwallet->mapExtAccounts.end())
                throw JSONRPCError(RPC_WALLET_ERROR, _("Unknown account."));

            sea = mi->second;
            nChain = sea->nActiveExternal;
            if (nChain < sea->vExtKeys.size())
                sek = sea->vExtKeys[nChain];
        } else
        {
            CKeyID keyId;
            ExtractExtKeyId(sInKey, keyId, CChainParams::EXT_KEY_HASH);
            
            ExtKeyAccountMap::iterator mi = pwallet->mapExtAccounts.begin();
            for (; mi != pwallet->mapExtAccounts.end(); ++mi)
            {
                sea = mi->second;
                for (uint32_t i = 0; i < sea->vExtKeyIDs.size(); ++i)
                {
                    if (sea->vExtKeyIDs[i] != keyId)
                        continue;
                    nChain = i;
                    sek = sea->vExtKeys[i];
                };
                if (sek)
                    break;
            };
        };
        
        if (!sek)
            throw JSONRPCError(RPC_WALLET_ERROR, _("Unknown chain."));
        
        uint32_t idIndex;
        if (fAddToAddressBook)
        {
            if (0 != pwallet->ExtKeyGetIndex(sea, idIndex))
                throw JSONRPCError(RPC_WALLET_ERROR, _("ExtKeyGetIndex failed."));
        };
        CHDWalletDB wdb(pwallet->strWalletFile, "r+");
        
        uint32_t nChildIn = (uint32_t)nStart;
        CPubKey newKey;
        for (int i = nStart; i <= nEnd; ++i)
        {
            nChildIn = (uint32_t)i;
            uint32_t nChildOut = 0;
            if (0 != sek->DeriveKey(newKey, nChildIn, nChildOut, fHardened))
                throw JSONRPCError(RPC_WALLET_ERROR, "DeriveKey failed.");
            
            if (nChildIn != nChildOut)
                LogPrintf("Warning: %s - DeriveKey skipped key %d.\n", __func__, nChildIn);
            
            if (fHardened)
                SetHardenedBit(nChildOut);
            
            CKeyID idk = newKey.GetID();
            result.push_back(CBitcoinAddress(idk).ToString());
            
            if (fSave)
            {
                CEKAKey ak(nChain, nChildOut);
                if (1 != sea->HaveKey(idk, false, ak))
                {
                    if (0 != pwallet->ExtKeySaveKey(sea, idk, ak))
                        throw JSONRPCError(RPC_WALLET_ERROR, "ExtKeySaveKey failed.");
                };
                
                if (fAddToAddressBook)
                {
                    std::vector<uint32_t> vPath;
                    vPath.push_back(idIndex); // first entry is the index to the account / master key
                    
                    if (0 == AppendChainPath(sek, vPath))
                        vPath.push_back(nChildOut);
                    else
                        vPath.clear();
                    
                    std::string strAccount = "";
                    pwallet->SetAddressBook(&wdb, idk, strAccount, "receive", vPath, false);
                };
            };
        };
    }
    
    return result;
}

UniValue clearwallettransactions(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "clearwallettransactions [remove_all]\n"
            "[remove_all] remove all transactions.\n"
            "Delete transactions from the wallet.\n"
            "By default removes only failed stakes.\n"
            "Wallet must be unlocked.\n"
            "Warning: Backup your wallet first!");
    
    CHDWallet *pwallet = GetHDWallet();
    EnsureWalletIsUnlocked(pwallet);
    
    bool fRemoveAll = false;
    if (request.params.size() > 0)
    {
        std::string s = request.params[0].get_str();
        
        if (!part::GetStringBool(s, fRemoveAll))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown argument for remove_all: %s.", s.c_str()));
    };
    
    int rv;
    size_t nRemoved = 0;
    size_t nRecordsRemoved = 0;
    
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        
        CHDWalletDB wdb(pwallet->strWalletFile);
        if (!wdb.TxnBegin())
            throw std::runtime_error("TxnBegin failed.");
        
        Dbc *pcursor = wdb.GetTxnCursor();
        if (!pcursor)
            throw std::runtime_error("GetTxnCursor failed.");
        
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        
        std::map<uint256, CWalletTx>::iterator itw;
        std::string strType;
        uint256 hash;
        uint32_t fFlags = DB_SET_RANGE;
        ssKey << std::string("tx");
        while (wdb.ReadKeyAtCursor(pcursor, ssKey, fFlags) == 0)
        {
            fFlags = DB_NEXT;
            
            ssKey >> strType;
            if (strType != "tx")
                break;
            ssKey >> hash;
            
            if (!fRemoveAll)
            {
                if ((itw = pwallet->mapWallet.find(hash)) == pwallet->mapWallet.end())
                {
                    LogPrintf("Warning: %s - tx not found in mapwallet! %s.\n", __func__, hash.ToString());
                    continue; // err on the side of caution
                };
                
                CWalletTx *pcoin = &itw->second;
                if (!pcoin->IsCoinStake() || !pcoin->isAbandoned())
                    continue;
            };
            
            //if (0 != pwallet->UnloadTransaction(hash))
            //    throw std::runtime_error("UnloadTransaction failed.");
            pwallet->UnloadTransaction(hash); // ignore failure
            
            if ((rv = pcursor->del(0)) != 0)
                throw std::runtime_error("pcursor->del failed.");
            
            nRemoved++;
        };
        
        if (fRemoveAll)
        {
            fFlags = DB_SET_RANGE;
            ssKey.clear();
            ssKey << std::string("rtx");
            while (wdb.ReadKeyAtCursor(pcursor, ssKey, fFlags) == 0)
            {
                fFlags = DB_NEXT;
                
                ssKey >> strType;
                if (strType != "rtx")
                    break;
                ssKey >> hash;
                
                pwallet->UnloadTransaction(hash); // ignore failure
                
                if ((rv = pcursor->del(0)) != 0)
                    throw std::runtime_error("pcursor->del failed.");
                
                // TODO: Remove CStoredTransaction
                
                nRecordsRemoved++;
            };
        };
        
        pcursor->close();
        if (!wdb.TxnCommit())
        {
            throw std::runtime_error("TxnCommit failed.");
        };
    }
    
    UniValue result(UniValue::VOBJ);
    
    result.push_back(Pair("transactions_removed", (int)nRemoved));
    result.push_back(Pair("records_removed", (int)nRecordsRemoved));
    
    return result;
}

UniValue filtertransactions(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "filtertransactions [offset] [count]\n"
            "List transactions.");
    
    throw std::runtime_error("TODO");
    
    CHDWallet *pwallet = GetHDWallet();
    
    UniValue result(UniValue::VARR);
    
    return result;
}


enum SortCodes
{
    SRT_LABEL_ASC,
    SRT_LABEL_DESC,
};

class AddressComp {
public:
    int nSortCode;
    AddressComp(int nSortCode_) : nSortCode(nSortCode_) {}
    bool operator() (
        const std::map<CTxDestination, CAddressBookData>::iterator a,
        const std::map<CTxDestination, CAddressBookData>::iterator b) const
    { 
        switch (nSortCode)
        {
            case SRT_LABEL_DESC:
                return b->second.name.compare(a->second.name) < 0;
            default:
                break;
        };
        //default: case SRT_LABEL_ASC:
        return a->second.name.compare(b->second.name) < 0;
    }
};

UniValue filteraddresses(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 6)
        throw std::runtime_error(
            "filteraddresses [offset] [count] [sort_code] [match_str] [match_owned] [show_path]\n"
            "filteraddresses [offset] [count] will list 'count' addresses starting from 'offset'\n"
            "filteraddresses -1 will count addresses\n"
            "[sort_code] 0 sort by label ascending, 1 sort by label descending, default 0\n"
            "[match_str] filter by label\n"
            "[match_owned] 0 off, 1 owned, 2 non-owned, default 0\n"
            "List addresses.");
    
    CHDWallet *pwallet = GetHDWallet();
    
    int nOffset = 0, nCount = 0x7FFFFFFF;
    if (request.params.size() > 0)
        nOffset = request.params[0].get_int();
    
    std::map<CTxDestination, CAddressBookData>::iterator it;
    if (request.params.size() == 1 && nOffset == -1)
    {
        LOCK(pwallet->cs_wallet);
        // count addresses
        UniValue result(UniValue::VOBJ);
        
        result.push_back(Pair("total", (int)pwallet->mapAddressBook.size()));
        
        int nReceive = 0, nSend = 0;
        for (it = pwallet->mapAddressBook.begin(); it != pwallet->mapAddressBook.end(); ++it)
        {
            if (it->second.nOwned == 0)
            {
                CBitcoinAddress address(it->first);
                it->second.nOwned = pwallet->HaveAddress(address) ? 1 : 2;
            };
            
            if (it->second.nOwned == 1)
                nReceive++;
            else
            if (it->second.nOwned == 2)
                nSend++;
        };
        
        result.push_back(Pair("num_receive", nReceive));
        result.push_back(Pair("num_send", nSend));
        
        return result;
    };
    
    if (request.params.size() > 1)
        nCount = request.params[1].get_int();
    
    if (nOffset < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "offset must be 0 or greater.");
    if (nCount < 1)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be 1 or greater.");
    
    
    // TODO: Make better
    int nSortCode = SRT_LABEL_ASC;
    if (request.params.size() > 2)
    {
        std::string sCode = request.params[2].get_str();
        if (sCode == "0")
            nSortCode = SRT_LABEL_ASC;
        else
        if (sCode == "1")
            nSortCode = SRT_LABEL_DESC;
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort_code.");
    };
    
    int nMatchOwned = 0; // 0 off/all, 1 owned, 2 non-owned
    int nMatchMode = 0; // 1 contains
    int nShowPath = 1;
    
    std::string sMatch;
    if (request.params.size() > 3)
        sMatch = request.params[3].get_str();
    
    if (sMatch != "")
        nMatchMode = 1;
    
    
    if (request.params.size() > 4)
    {
        std::string s = request.params[4].get_str();
        if (s != "")
            nMatchOwned = std::stoi(s);
    };
    
    if (request.params.size() > 5)
    {
        std::string s = request.params[5].get_str();
        bool fTemp;
        if (!part::GetStringBool(s, fTemp))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown argument for show_path: %s.", s.c_str()));
        nShowPath = !fTemp ? 0 : nShowPath;
    };
    
    
    UniValue result(UniValue::VARR);
    
    {
        LOCK(pwallet->cs_wallet);
        
        CHDWalletDB wdb(pwallet->strWalletFile, "r+");
        
        if (nOffset >= (int)pwallet->mapAddressBook.size())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("offset is beyond last address (%d).", nOffset));
        std::vector<std::map<CTxDestination, CAddressBookData>::iterator> vitMapAddressBook;
        vitMapAddressBook.reserve(pwallet->mapAddressBook.size());
        
        for (it = pwallet->mapAddressBook.begin(); it != pwallet->mapAddressBook.end(); ++it)
        {
            if (it->second.nOwned == 0)
            {
                CBitcoinAddress address(it->first);
                it->second.nOwned = pwallet->HaveAddress(address) ? 1 : 2;
            };
            
            if (nMatchOwned && it->second.nOwned != nMatchOwned)
                continue;
            
            if (nMatchMode)
            {
                if (!part::stringsMatchI(it->second.name, sMatch, nMatchMode-1))
                    continue;
            };
            
            vitMapAddressBook.push_back(it);
        };
        
        std::sort(vitMapAddressBook.begin(), vitMapAddressBook.end(), AddressComp(nSortCode));
        
        std::map<uint32_t, std::string> mapKeyIndexCache;
        std::vector<std::map<CTxDestination, CAddressBookData>::iterator>::iterator vit;
        int nEntries = 0;
        for (vit = vitMapAddressBook.begin()+nOffset;
            vit != vitMapAddressBook.end() && nEntries < nCount; ++vit)
        {
            auto &item = *vit;
            UniValue entry(UniValue::VOBJ);
            
            CBitcoinAddress address(item->first);
            entry.push_back(Pair("address", address.ToString()));
            entry.push_back(Pair("label", item->second.name));
            entry.push_back(Pair("owned", item->second.nOwned == 1 ? "true" : "false"));
            
            if (nShowPath > 0)
            {
                if (item->second.vPath.size() > 0)
                {
                    uint32_t index = item->second.vPath[0];
                    std::map<uint32_t, std::string>::iterator mi = mapKeyIndexCache.find(index);
                    
                    if (mi != mapKeyIndexCache.end())
                    {
                        entry.push_back(Pair("root", mi->second));
                    } else
                    {
                        CKeyID accId;
                        if (!wdb.ReadExtKeyIndex(index, accId))
                        {
                            entry.push_back(Pair("root", "error"));
                        } else
                        {
                            CBitcoinAddress addr;
                            addr.Set(accId, CChainParams::EXT_ACC_HASH);
                            std::string sTmp = addr.ToString();
                            entry.push_back(Pair("root", sTmp));
                            mapKeyIndexCache[index] = sTmp;
                        };
                    };
                };
                
                if (item->second.vPath.size() > 1)
                {
                    std::string sPath;
                    if (0 == PathToString(item->second.vPath, sPath, '\'', 1))
                        entry.push_back(Pair("path", sPath));
                };
            };
            
            result.push_back(entry);
            nEntries++;
        };
    } // cs_wallet
    
    return result;
}

UniValue manageaddressbook(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4)
        throw std::runtime_error(
            "manageaddressbook <action> <address> [label] [purpose]\n"
            "Manage the address book."
            "\nArguments:\n"
            "1. \"action\"      (string, required) 'add/edit/del/info/newsend' The action to take.\n"
            "2. \"address\"     (string, required) The address to affect.\n"
            "3. \"label\"       (string, optional) Optional label.\n"
            "4. \"purpose\"     (string, optional) Optional purpose label.\n");
    
    CHDWallet *pwallet = GetHDWallet();
    
    
    std::string sAction = request.params[0].get_str();
    std::string sAddress = request.params[1].get_str();
    std::string sLabel, sPurpose;
    
    if (sAction != "info")
        EnsureWalletIsUnlocked(pwallet);
    
    bool fHavePurpose = false;
    if (request.params.size() > 2)
        sLabel = request.params[2].get_str();
    if (request.params.size() > 3)
    {
        sPurpose = request.params[3].get_str();
        fHavePurpose = true;
    };
    
    CBitcoinAddress address(sAddress);
    
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Invalid Particl address."));
    
    CTxDestination dest = address.Get();
    
    std::map<CTxDestination, CAddressBookData>::iterator mabi;
    mabi = pwallet->mapAddressBook.find(dest);
    
    std::vector<uint32_t> vPath;
    
    UniValue objDestData(UniValue::VOBJ);
    
    if (sAction == "add")
    {
        if (mabi != pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is recorded in the address book."), sAddress));
        
        if (!pwallet->SetAddressBook(NULL, dest, sLabel, sPurpose, vPath, true))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");
    } else
    if (sAction == "edit")
    {
        if (request.params.size() < 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Need a parameter to change."));
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));
        
        if (!pwallet->SetAddressBook(NULL, dest, sLabel,
            fHavePurpose ? sPurpose : mabi->second.purpose, mabi->second.vPath, true))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");
        
        sLabel = mabi->second.name;
        sPurpose = mabi->second.purpose;
        
        for (const auto &pair : mabi->second.destdata)
            objDestData.push_back(Pair(pair.first, pair.second));
        
    } else
    if (sAction == "del")
    {
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));
        sLabel = mabi->second.name;
        sPurpose = mabi->second.purpose;
        
        if (!pwallet->DelAddressBook(dest))
            throw JSONRPCError(RPC_WALLET_ERROR, "DelAddressBook failed.");
    } else
    if (sAction == "info")
    {
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));
        
        UniValue result(UniValue::VOBJ);
    
        result.push_back(Pair("action", sAction));
        result.push_back(Pair("address", sAddress));
        
        result.push_back(Pair("label", mabi->second.name));
        result.push_back(Pair("purpose", mabi->second.purpose));
        
        if (mabi->second.nOwned == 0)
            mabi->second.nOwned = pwallet->HaveAddress(address) ? 1 : 2;
        
        result.push_back(Pair("owned", mabi->second.nOwned == 1 ? "true" : "false"));
        
        if (mabi->second.vPath.size() > 1)
        {
            std::string sPath;
            if (0 == PathToString(mabi->second.vPath, sPath, '\'', 1))
                result.push_back(Pair("path", sPath));
        };
        
        for (const auto &pair : mabi->second.destdata)
            objDestData.push_back(Pair(pair.first, pair.second));
        if (objDestData.size() > 0)
            result.push_back(Pair("destdata", objDestData));
        
        result.push_back(Pair("result", "success"));
        
        return result;
    } else
    if (sAction == "newsend")
    {
        // Only update the purpose field if address does not yet exist
        if (mabi != pwallet->mapAddressBook.end())
            sPurpose = "";// "" means don't change purpose
        
        if (!pwallet->SetAddressBook(dest, sLabel, sPurpose))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");
        
        if (mabi != pwallet->mapAddressBook.end())
            sPurpose = mabi->second.purpose;
    } else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Unknown action, must be one of 'add/edit/del'."));
    };
    
    UniValue result(UniValue::VOBJ);
    
    result.push_back(Pair("action", sAction));
    result.push_back(Pair("address", sAddress));
    
    if (sLabel.size() > 0)
        result.push_back(Pair("label", sLabel));
    if (sPurpose.size() > 0)
        result.push_back(Pair("purpose", sPurpose));
    if (objDestData.size() > 0)
        result.push_back(Pair("destdata", objDestData));
    
    result.push_back(Pair("result", "success"));
    
    return result;
}

UniValue setvote(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 4)
        throw std::runtime_error(
            "setvote <proposal> <option> <height_start> <height_end>\n"
            "Set voting token.\n"
            "Proposal is the proposal to vote on.\n"
            "Option is the option to vote for.\n"
            "The last added option valid for a range will be applied.\n"
            "Wallet will include this token in staked blocks from height_start to height_end.\n"
            "Set proposal and/or option to 0 to stop voting.\n");
    
    CHDWallet *pwallet = GetHDWallet();
    EnsureWalletIsUnlocked(pwallet);
    
    uint32_t issue = request.params[0].get_int();
    uint32_t option = request.params[1].get_int();
    
    if (issue > 0xFFFF)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Proposal out of range."));
    if (option > 0xFFFF)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Option out of range."));

    int nStartHeight = request.params[2].get_int();
    int nEndHeight = request.params[3].get_int();
    
    if (nEndHeight < nStartHeight)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("height_end must be after height_start."));
    
    uint32_t voteToken = issue | (option << 16);
    
    {
        LOCK(pwallet->cs_wallet);
        
        CHDWalletDB wdb(pwallet->strWalletFile, "r+");
        
        std::vector<CVoteToken> vVoteTokens;
    
        wdb.ReadVoteTokens(vVoteTokens);
        
        CVoteToken v(voteToken, nStartHeight, nEndHeight, GetTime());
        vVoteTokens.push_back(v);
        
        if (!wdb.WriteVoteTokens(vVoteTokens))
            throw JSONRPCError(RPC_WALLET_ERROR, "WriteVoteTokens failed.");
        
        pwallet->LoadVoteTokens(&wdb);
    }
    
    UniValue result(UniValue::VOBJ);
    
    if (issue < 1)
        result.push_back(Pair("result", _("Cleared vote token.")));
    else
        result.push_back(Pair("result", strprintf(_("Voting for option %u on proposal %u"), option, issue)));
    
    result.push_back(Pair("from_height", nStartHeight));
    result.push_back(Pair("to_height", nEndHeight));
    
    return result;
}

UniValue votehistory(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "votehistory [current_only]\n"
            "Display voting history.\n");
    
    CHDWallet *pwallet = GetHDWallet();
    
    UniValue result(UniValue::VARR);
    
    if (request.params.size() > 0)
    {
        std::string s = request.params[0].get_str();
        if (part::IsStringBoolPositive(s))
        {
            UniValue vote(UniValue::VOBJ);
            
            int nNextHeight = chainActive.Height() + 1;
            
            for (auto i = pwallet->vVoteTokens.size(); i-- > 0; )
            {
                auto &v = pwallet->vVoteTokens[i];
                if (v.nEnd < nNextHeight
                    || v.nStart > nNextHeight)
                    continue;
                
                if ((v.nToken >> 16) < 1
                    || (v.nToken & 0xFFFF) < 1)
                    continue;
                UniValue vote(UniValue::VOBJ);
                vote.push_back(Pair("proposal", (int)(v.nToken & 0xFFFF)));
                vote.push_back(Pair("option", (int)(v.nToken >> 16)));
                vote.push_back(Pair("from_height", v.nStart));
                vote.push_back(Pair("to_height", v.nEnd));
                result.push_back(vote);
            };
            return result;
        };
    };
    
    std::vector<CVoteToken> vVoteTokens;
    {
        LOCK(pwallet->cs_wallet);
        
        CHDWalletDB wdb(pwallet->strWalletFile, "r+");
        wdb.ReadVoteTokens(vVoteTokens);
    }
    
    for (auto i = vVoteTokens.size(); i-- > 0; )
    {
        auto &v = vVoteTokens[i];
        UniValue vote(UniValue::VOBJ);
        vote.push_back(Pair("proposal", (int)(v.nToken & 0xFFFF)));
        vote.push_back(Pair("option", (int)(v.nToken >> 16)));
        vote.push_back(Pair("from_height", v.nStart));
        vote.push_back(Pair("to_height", v.nEnd));
        vote.push_back(Pair("added", v.nTimeAdded));
        result.push_back(vote);
    };
    
    return result;
}

UniValue tallyvotes(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error(
            "tallyvotes <proposal> <height_start> <height_end>\n"
            "count votes.\n");
    
    int issue = request.params[0].get_int();
    if (issue < 1 || issue >= (1 << 16))
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Proposal out of range."));
    
    int nStartHeight = request.params[1].get_int();
    int nEndHeight = request.params[2].get_int();
    
    CBlock block;
    const Consensus::Params& consensusParams = Params().GetConsensus();
    
    std::map<int, int> mapVotes;
    std::pair<std::map<int, int>::iterator, bool> ri;
    
    int nBlocks = 0;
    CBlockIndex *pindex = chainActive.Tip();
    if (pindex)
    do
    {
        if (pindex->nHeight < nStartHeight)
            break;
        if (pindex->nHeight <= nEndHeight)
        {
            if (!ReadBlockFromDisk(block, pindex, consensusParams))
                continue;
            
            if (block.vtx.size() < 1
                || !block.vtx[0]->IsCoinStake())
                continue;
            
            std::vector<uint8_t> &vData = ((CTxOutData*)block.vtx[0]->vpout[0].get())->vData;
            if (vData.size() < 9 || vData[4] != DO_VOTE)
            {
                ri = mapVotes.insert(std::pair<int, int>(0, 1));
                if (!ri.second) ri.first->second++;
            } else
            {
                uint32_t voteToken;
                memcpy(&voteToken, &vData[5], 4);
                int option = 0; // default to abstain
                
                // count only if related to current issue:
                if ((int) (voteToken & 0xFFFF) == issue)
                    option = (voteToken >> 16) & 0xFFFF;
                
                ri = mapVotes.insert(std::pair<int, int>(option, 1));
                if (!ri.second) ri.first->second++;
            };
            
            nBlocks++;
        };
    } while ((pindex = pindex->pprev));
    
    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("proposal", issue));
    result.push_back(Pair("height_start", nStartHeight));
    result.push_back(Pair("height_end", nEndHeight));
    result.push_back(Pair("blocks_counted", nBlocks));
    
    float fnBlocks = (float) nBlocks;
    for (auto &i : mapVotes)
    {
        std::string sKey = i.first == 0 ? "Abstain" : strprintf("Option %d", i.first);
        result.push_back(Pair(sKey, strprintf("%d, %.02f%%", i.second, ((float) i.second / fnBlocks) * 100.0)));
    };
    
    return result;
};

UniValue getstakinginfo(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getstakinginfo\n"
            "Returns an object containing staking-related information.");
    
    CHDWallet *pwallet = GetHDWallet();
    
    UniValue obj(UniValue::VOBJ);
    
    int64_t nTipTime;
    float rCoinYearReward;
    CAmount nMoneySupply;
    {
        LOCK(cs_main);
        nTipTime = chainActive.Tip()->nTime;
        rCoinYearReward = Params().GetCoinYearReward(nTipTime) / CENT;
        nMoneySupply = chainActive.Tip()->nMoneySupply;
    }
    
    uint64_t nWeight = pwallet->GetStakeWeight();

    uint64_t nNetworkWeight = GetPoSKernelPS();
    
    bool fStaking = nLastCoinStakeSearchTime && nWeight && fIsStaking;
    uint64_t nExpectedTime = fStaking ? (Params().GetTargetSpacing() * nNetworkWeight / nWeight) : 0;

    obj.push_back(Pair("enabled", GetBoolArg("-staking", true)));
    obj.push_back(Pair("staking", fStaking));
    obj.push_back(Pair("errors", GetWarnings("statusbar")));

    obj.push_back(Pair("percentyearreward", rCoinYearReward));
    obj.push_back(Pair("moneysupply", ValueFromAmount(nMoneySupply)));

    if (pwallet->nUserDevFundCedePercent > 0)
        obj.push_back(Pair("userfoundationdonationpercent", pwallet->nUserDevFundCedePercent));

    const DevFundSettings *pDevFundSettings = Params().GetDevFundSettings(nTipTime);
    if (pDevFundSettings && pDevFundSettings->nMinDevStakePercent > 0)
        obj.push_back(Pair("foundationdonationpercent", pDevFundSettings->nMinDevStakePercent));
    

    obj.push_back(Pair("currentblocksize", (uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblocktx", (uint64_t)nLastBlockTx));
    obj.push_back(Pair("pooledtx", (uint64_t)mempool.size()));

    obj.push_back(Pair("difficulty", GetDifficulty()));
    obj.push_back(Pair("search-interval", (int)nLastCoinStakeSearchInterval));

    obj.push_back(Pair("weight", (uint64_t)nWeight));
    obj.push_back(Pair("netstakeweight", (uint64_t)nNetworkWeight));

    obj.push_back(Pair("expectedtime", nExpectedTime));
    
    return obj;
};


UniValue listunspentanon(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listunspentanon ( minconf maxconf  [\"addresses\",...] [include_unsafe] )\n"
            "\nReturns array of unspent transaction anon outputs\n"
            "with between minconf and maxconf (inclusive) confirmations.\n"
            "Optionally filter to only include txouts paid to specified addresses.\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) The minimum confirmations to filter\n"
            "2. maxconf          (numeric, optional, default=9999999) The maximum confirmations to filter\n"
            "3. \"addresses\"    (string) A json array of particl addresses to filter\n"
            "    [\n"
            "      \"address\"   (string) particl address\n"
            "      ,...\n"
            "    ]\n"
            "4. include_unsafe (bool, optional, default=true) Include outputs that are not safe to spend\n"
            "                  because they come from unconfirmed untrusted transactions or unconfirmed\n"
            "                  replacement transactions (cases where we are less sure that a conflicting\n"
            "                  transaction won't be mined).\n"
            "\nResult\n"
            "[                   (array of json object)\n"
            "  {\n"
            "    \"txid\" : \"txid\",          (string) the transaction id \n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"address\" : \"address\",    (string) the particl address\n"
            "    \"account\" : \"account\",    (string) DEPRECATED. The associated account, or \"\" for the default account\n"
            //"    \"scriptPubKey\" : \"key\",   (string) the script key\n"
            "    \"amount\" : x.xxx,         (numeric) the transaction output amount in " + CURRENCY_UNIT + "\n"
            "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
            //"    \"redeemScript\" : n        (string) The redeemScript if scriptPubKey is P2SH\n"
            //"    \"spendable\" : xxx,        (bool) Whether we have the private keys to spend this output\n"
            //"    \"solvable\" : xxx          (bool) Whether we know how to spend this output, ignoring the lack of keys\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples\n"
            + HelpExampleCli("listunspentanon", "")
            + HelpExampleCli("listunspentanon", "6 9999999 \"[\\\"PfqK97PXYfqRFtdYcZw82x3dzPrZbEAcYa\\\",\\\"Pka9M2Bva8WetQhQ4ngC255HAbMJf5P5Dc\\\"]\"")
            + HelpExampleRpc("listunspentanon", "6, 9999999 \"[\\\"PfqK97PXYfqRFtdYcZw82x3dzPrZbEAcYa\\\",\\\"Pka9M2Bva8WetQhQ4ngC255HAbMJf5P5Dc\\\"]\"")
        );
    
    CHDWallet *pwallet = GetHDWallet();
    
    int nMinDepth = 1;
    if (request.params.size() > 0 && !request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    std::set<CBitcoinAddress> setAddress;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CBitcoinAddress address(input.get_str());
            if (!address.IsValidStealthAddress())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Particl stealth address: ")+input.get_str());
            if (setAddress.count(address))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ")+input.get_str());
           setAddress.insert(address);
        }
    }
    
    bool include_unsafe = true;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }
    
    UniValue results(UniValue::VARR);
    std::vector<COutputR> vecOutputs;
    assert(pwalletMain != NULL);
    LOCK2(cs_main, pwalletMain->cs_wallet);
    
    
    // TODO: filter on stealth address
    pwallet->AvailableAnonCoins(vecOutputs, !include_unsafe, NULL, true);
    
    for (const auto &out : vecOutputs)
    {
        if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
            continue;
        
        const COutputRecord *pout = out.rtx->second.GetOutput(out.i);
        
        if (!pout)
        {
            LogPrintf("%s: ERROR - Missing output %s %d\n", __func__, out.txhash.ToString(), out.i);
            continue;
        };
        
        CAmount nValue = pout->nValue;
        
        
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("txid", out.txhash.GetHex()));
        entry.push_back(Pair("vout", out.i));

        entry.push_back(Pair("amount", ValueFromAmount(nValue)));
        entry.push_back(Pair("confirmations", out.nDepth));
        //entry.push_back(Pair("spendable", out.fSpendable));
        //entry.push_back(Pair("solvable", out.fSolvable));
        results.push_back(entry);
    }

    return results;
};

UniValue listunspentblind(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listunspentblind ( minconf maxconf  [\"addresses\",...] [include_unsafe] )\n"
            "\nReturns array of unspent transaction blind outputs\n"
            "with between minconf and maxconf (inclusive) confirmations.\n"
            "Optionally filter to only include txouts paid to specified addresses.\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) The minimum confirmations to filter\n"
            "2. maxconf          (numeric, optional, default=9999999) The maximum confirmations to filter\n"
            "3. \"addresses\"    (string) A json array of particl addresses to filter\n"
            "    [\n"
            "      \"address\"   (string) particl address\n"
            "      ,...\n"
            "    ]\n"
            "4. include_unsafe (bool, optional, default=true) Include outputs that are not safe to spend\n"
            "                  because they come from unconfirmed untrusted transactions or unconfirmed\n"
            "                  replacement transactions (cases where we are less sure that a conflicting\n"
            "                  transaction won't be mined).\n"
            "\nResult\n"
            "[                   (array of json object)\n"
            "  {\n"
            "    \"txid\" : \"txid\",          (string) the transaction id \n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"address\" : \"address\",    (string) the particl address\n"
            "    \"account\" : \"account\",    (string) DEPRECATED. The associated account, or \"\" for the default account\n"
            "    \"scriptPubKey\" : \"key\",   (string) the script key\n"
            "    \"amount\" : x.xxx,         (numeric) the transaction output amount in " + CURRENCY_UNIT + "\n"
            "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
            "    \"redeemScript\" : n        (string) The redeemScript if scriptPubKey is P2SH\n"
            //"    \"spendable\" : xxx,        (bool) Whether we have the private keys to spend this output\n"
            //"    \"solvable\" : xxx          (bool) Whether we know how to spend this output, ignoring the lack of keys\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples\n"
            + HelpExampleCli("listunspentblind", "")
            + HelpExampleCli("listunspentblind", "6 9999999 \"[\\\"PfqK97PXYfqRFtdYcZw82x3dzPrZbEAcYa\\\",\\\"Pka9M2Bva8WetQhQ4ngC255HAbMJf5P5Dc\\\"]\"")
            + HelpExampleRpc("listunspentblind", "6, 9999999 \"[\\\"PfqK97PXYfqRFtdYcZw82x3dzPrZbEAcYa\\\",\\\"Pka9M2Bva8WetQhQ4ngC255HAbMJf5P5Dc\\\"]\"")
        );
    
    CHDWallet *pwallet = GetHDWallet();
    
    int nMinDepth = 1;
    if (request.params.size() > 0 && !request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    std::set<CBitcoinAddress> setAddress;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CBitcoinAddress address(input.get_str());
            if (!address.IsValidStealthAddress())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Particl stealth address: ")+input.get_str());
            if (setAddress.count(address))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ")+input.get_str());
           setAddress.insert(address);
        }
    }
    
    bool include_unsafe = true;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }
    
    UniValue results(UniValue::VARR);
    std::vector<COutputR> vecOutputs;
    assert(pwalletMain != NULL);
    LOCK2(cs_main, pwalletMain->cs_wallet);
    
    pwallet->AvailableBlindedCoins(vecOutputs, !include_unsafe, NULL, true);
    
    for (const auto &out : vecOutputs)
    {
        if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
            continue;
        
        const COutputRecord *pout = out.rtx->second.GetOutput(out.i);
        
        if (!pout)
        {
            LogPrintf("%s: ERROR - Missing output %s %d\n", __func__, out.txhash.ToString(), out.i);
            continue;
        };
        
        CAmount nValue = pout->nValue;
        
        CTxDestination address;
        const CScript *scriptPubKey = &pout->scriptPubKey;
        bool fValidAddress;
        
        fValidAddress = ExtractDestination(*scriptPubKey, address);
        if (setAddress.size() && (!fValidAddress || !setAddress.count(address)))
            continue;
        
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("txid", out.txhash.GetHex()));
        entry.push_back(Pair("vout", out.i));

        if (fValidAddress) {
            entry.push_back(Pair("address", CBitcoinAddress(address).ToString()));

            if (pwallet->mapAddressBook.count(address))
                entry.push_back(Pair("account", pwallet->mapAddressBook[address].name));

            if (scriptPubKey->IsPayToScriptHash()) {
                const CScriptID& hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwallet->GetCScript(hash, redeemScript))
                    entry.push_back(Pair("redeemScript", HexStr(redeemScript.begin(), redeemScript.end())));
            }
        }

        entry.push_back(Pair("scriptPubKey", HexStr(scriptPubKey->begin(), scriptPubKey->end())));
        entry.push_back(Pair("amount", ValueFromAmount(nValue)));
        entry.push_back(Pair("confirmations", out.nDepth));
        //entry.push_back(Pair("spendable", out.fSpendable));
        //entry.push_back(Pair("solvable", out.fSolvable));
        results.push_back(entry);
    }

    return results;
};
/*
UniValue gettransactionsummary(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "gettransactionsummary <txhash>\n"
            "Returns a summary of a transaction in the wallet.");
    
    CHDWallet *pwallet = GetHDWallet();
    
    UniValue obj(UniValue::VOBJ);
    
    uint256 hash;
    hash.SetHex(request.params[0].get_str());
    
    {
        LOCK(pwallet->cs_wallet);
        
        MapRecords_t::const_iterator mri;
        MapWallet_t::const_iterator mwi;
        
        if ((mwi = pwallet->mapWallet.find(hash)) != pwallet->mapWallet.end())
        {
            const CWalletTx &wtx = mwi->second;
            
            obj.push_back(Pair("time", (int64_t)wtx.nTimeSmart));
            
        } else
        if ((mri = pwallet->mapRecords.find(hash)) != pwallet->mapRecords.end())
        {
            const CTransactionRecord &rtx = mri->second;
            
            obj.push_back(Pair("time", std::min((int64_t)rtx.nTimeReceived, (int64_t)rtx.nBlockTime)));
        } else
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Unknown transaction."));
        };
    }
    
    obj.push_back(Pair("part_balance", ""));
    obj.push_back(Pair("blind_balance", ""));
    obj.push_back(Pair("anon_balance", ""));
    
    
    return obj;
};
*/


static int AddOutput(uint8_t nType, std::vector<CTempRecipient> &vecSend, const CTxDestination &address, CAmount nValue,
    bool fSubtractFeeFromAmount, std::string &sNarr, std::string &sError)
{
    CTempRecipient r;
    r.nType = nType;
    r.nAmount = nValue;
    r.address = address;
    r.sNarration = sNarr;
    
    vecSend.push_back(r);
    return 0;
};

static UniValue SendToInner(const JSONRPCRequest &request, OutputTypes typeIn, OutputTypes typeOut)
{
    CHDWallet *pwallet = GetHDWallet();
    EnsureWalletIsUnlocked(pwallet);
    
    if (typeOut == OUTPUT_RINGCT && Params().NetworkID() == "main")
        throw std::runtime_error("Disabled on mainnet.");
    
    CAmount nTotal = 0;
    
    std::vector<CTempRecipient> vecSend;
    std::string sError;
    
    
    size_t nCommentOfs = 2;
    size_t nRingSizeOfs = 6;
    size_t nTestFeeOfs = 99;
    
    if (request.params[0].isArray())
    {
        const UniValue &outputs = request.params[0].get_array();
        
        for (size_t k = 0; k < outputs.size(); ++k)
        {
            if (!outputs[k].isObject())
                throw JSONRPCError(RPC_TYPE_ERROR, "Not an object");
            const UniValue &obj = outputs[k].get_obj();
            
            std::string sAddress;
            CAmount nAmount;
            
            if (obj.exists("address"))
                sAddress = obj["address"].get_str();
            else
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Must provide an address.");
            
            CBitcoinAddress address(sAddress);
            
            if (typeOut == OUTPUT_RINGCT
                && !address.IsValidStealthAddress())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Particl stealth address");
            
            if (!address.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Particl address");
                
            
            if (obj.exists("amount"))
                nAmount = AmountFromValue(obj["amount"]);
            else
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Must provide an address.");
            
            if (nAmount <= 0)
                throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
            nTotal += nAmount;
            
            bool fSubtractFeeFromAmount = false;
            if (obj.exists("subfee"))
                fSubtractFeeFromAmount = obj["subfee"].get_bool();
            
            std::string sNarr;
            if (obj.exists("narr"))
                sNarr = obj["narr"].get_str();
            
            if (0 != AddOutput(typeOut, vecSend, address.Get(), nAmount, fSubtractFeeFromAmount, sNarr, sError))
                throw JSONRPCError(RPC_MISC_ERROR, strprintf("AddOutput failed: %s.", sError));
        };
        nCommentOfs = 1;
        nRingSizeOfs = 3;
        nTestFeeOfs = 5;
    } else
    {
        std::string sAddress = request.params[0].get_str();
        CBitcoinAddress address(sAddress);
        
        if (typeOut == OUTPUT_RINGCT
            && !address.IsValidStealthAddress())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Particl stealth address");
        
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Particl address");

        CAmount nAmount = AmountFromValue(request.params[1]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
        nTotal += nAmount;
        
        bool fSubtractFeeFromAmount = false;
        if (request.params.size() > 4)
            fSubtractFeeFromAmount = request.params[4].get_bool();
        
        if (fSubtractFeeFromAmount)
            throw std::runtime_error("TODO");
        
        std::string sNarr;
        if (request.params.size() > 5)
        {
            sNarr = request.params[5].get_str();
            if (sNarr.length() > 24)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Narration can range from 1 to 24 characters.");
        };
        
        if (0 != AddOutput(typeOut, vecSend, address.Get(), nAmount, fSubtractFeeFromAmount, sNarr, sError))
            throw JSONRPCError(RPC_MISC_ERROR, strprintf("AddOutput failed: %s.", sError));
    };
    
    switch (typeIn)
    {
        case OUTPUT_STANDARD:
            if (nTotal > pwallet->GetBalance())
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
            break;
        case OUTPUT_CT:
            if (nTotal > pwallet->GetBlindBalance())
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient blinded funds");
            break;
        case OUTPUT_RINGCT:
            if (nTotal > pwallet->GetAnonBalance())
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient anon funds");
            break;
        default:
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown input type: %d.", typeIn));
    };
    
    // Wallet comments
    CWalletTx wtx;
    CTransactionRecord rtx;
    
    size_t nv = nCommentOfs;
    if (request.params.size() > nv && !request.params[nv].isNull())
    {
        std::string s = request.params[nv].get_str();
        part::TrimQuotes(s);
        if (!s.empty())
        {
            std::vector<uint8_t> v(s.begin(), s.end());
            wtx.mapValue["comment"] = s;
            rtx.mapValue[RTXVT_COMMENT] = v;
        };
    };
    nv++;
    if (request.params.size() > nv && !request.params[nv].isNull())
    {
        std::string s = request.params[nv].get_str();
        part::TrimQuotes(s);
        if (!s.empty())
        {
            std::vector<uint8_t> v(s.begin(), s.end());
            wtx.mapValue["to"] = s;
            rtx.mapValue[RTXVT_TO] = v;
        };
    };
    
    nv = nRingSizeOfs;
    size_t nRingSize = 4; // TODO: default size?
    if (request.params.size() > nv)
        nRingSize = request.params[nv].get_int();
    nv++;
    size_t nInputsPerSig = 64;
    if (request.params.size() > nv)
        nInputsPerSig = request.params[nv].get_int();
    
    bool fCheckFeeOnly = false;
    nv = nTestFeeOfs;
    if (request.params.size() > nv)
        fCheckFeeOnly = request.params[nv].get_bool();
    
    CReserveKey reservekey(pwallet);
    
    CAmount nFeeRet = 0;
    switch (typeIn)
    {
        case OUTPUT_STANDARD:
            if (0 != pwallet->AddStandardInputs(wtx, rtx, vecSend, !fCheckFeeOnly, nFeeRet, sError))
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("AddStandardInputs failed: %s.", sError));
            break;
        case OUTPUT_CT:
            if (0 != pwallet->AddBlindedInputs(wtx, rtx, vecSend, !fCheckFeeOnly, nFeeRet, sError))
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("AddBlindedInputs failed: %s.", sError));
            break;
        case OUTPUT_RINGCT:
            if (0 != pwallet->AddAnonInputs(wtx, rtx, vecSend, !fCheckFeeOnly, nRingSize, nInputsPerSig, nFeeRet, sError))
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("AddAnonInputs failed: %s.", sError));
            break;
        default:
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown input type: %d.", typeIn));
    };
    
    if (fCheckFeeOnly)
    {
        UniValue result(UniValue::VOBJ);
        result.push_back(Pair("fee", ValueFromAmount(nFeeRet)));
        return result;
    };
    
    CValidationState state;
    
    if (typeIn == OUTPUT_STANDARD && typeOut == OUTPUT_STANDARD)
    {
        if (!pwallet->CommitTransaction(wtx, reservekey, g_connman.get(), state))
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Transaction commit failed: %s", state.GetRejectReason()));
    } else
    {
        if (!pwallet->CommitTransaction(wtx, rtx, reservekey, g_connman.get(), state))
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Transaction commit failed: %s", state.GetRejectReason()));
    }
    
    UniValue vErrors(UniValue::VARR);
    if (!state.IsValid())
    {
        // This can happen if the mempool rejected the transaction.  Report
        // what happened in the "errors" response.
        vErrors.push_back(strprintf("Error: The transaction was rejected: %s", FormatStateMessage(state)));
        
        UniValue result(UniValue::VOBJ);
        result.push_back(Pair("txid", wtx.GetHash().GetHex()));
        result.push_back(Pair("errors", vErrors));
        return result;
    };
    
    pwallet->PostProcessTempRecipients(vecSend);
    
    return wtx.GetHash().GetHex();
}

static const char *TypeToWord(OutputTypes type)
{
    switch (type)
    {
        case OUTPUT_STANDARD:
            return "part";
        case OUTPUT_CT:
            return "blind";
        case OUTPUT_RINGCT:
            return "anon";
        default:
            break;
    };
    return "unknown";
};

static OutputTypes WordToType(std::string &s)
{
    if (s == "part")
        return OUTPUT_STANDARD;
    if (s == "blind")
        return OUTPUT_CT;
    if (s == "anon")
        return OUTPUT_RINGCT;
    return OUTPUT_NULL;
};

static std::string SendHelp(OutputTypes typeIn, OutputTypes typeOut)
{
    std::string rv;
    
    std::string cmd = std::string("send") + TypeToWord(typeIn) + "to" + TypeToWord(typeOut);
    
    rv = cmd + "\"address\" amount ( \"comment\" \"comment-to\" subtractfeefromamount, \"narration\"";
    if (typeIn == OUTPUT_RINGCT)
        rv += ", \"ringsize\", \"numsignatures\"";
    rv += ")\n";
    
    rv += "\nSend an amount of ";
    rv += typeIn == OUTPUT_RINGCT ? "anon" : typeIn == OUTPUT_CT ? "blinded" : "";
    rv += std::string(" part in a") + (typeOut == OUTPUT_RINGCT || typeOut == OUTPUT_CT ? " blinded" : "") + " payment to a given address"
        + (typeOut == OUTPUT_CT ? " in anon part": "") + ".\n";
    
    rv += HelpRequiringPassphrase();
    
    rv +=   "\nArguments:\n"
            "1. \"address\"     (string, required) The particl address to send to.\n"
            "2. \"amount\"      (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "3. \"comment\"     (string, optional) A comment used to store what the transaction is for. \n"
            "                            This is not part of the transaction, just kept in your wallet.\n"
            "4. \"comment_to\"  (string, optional) A comment to store the name of the person or organization \n"
            "                            to which you're sending the transaction. This is not part of the \n"
            "                            transaction, just kept in your wallet.\n"
            "5. subtractfeefromamount  (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "                            The recipient will receive less " + CURRENCY_UNIT + " than you enter in the amount field.\n"
            "6. \"narration\"   (string, optional) Up to 24 characters sent with the transaction.\n"
            "                            The narration is stored in the blockchain and is sent encrypted when destination is a stealth address and uncrypted otherwise.\n";
    if (typeIn == OUTPUT_RINGCT)
        rv +=
            "7. \"ringsize\"       (int, optional).\n"
            "8. \"inputs_per_sig\" (int, optional).\n";
    
    rv +=   
            "\nResult:\n"
            "\"txid\"           (string) The transaction id.\n";
    
    rv +=   "\nExamples:\n"
            + HelpExampleCli(cmd, "\"SPGyji8uZFip6H15GUfj6bsutRVLsCyBFL3P7k7T7MUDRaYU8GfwUHpfxonLFAvAwr2RkigyGfTgWMfzLAAP8KMRHq7RE8cwpEEekH\" 0.1");
    
    return rv;
};

UniValue sendparttoblind(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 6)
        throw std::runtime_error(SendHelp(OUTPUT_STANDARD, OUTPUT_CT));
    
    return SendToInner(request, OUTPUT_STANDARD, OUTPUT_CT);
}

UniValue sendparttoanon(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 6)
        throw std::runtime_error(SendHelp(OUTPUT_STANDARD, OUTPUT_RINGCT));
    
    return SendToInner(request, OUTPUT_STANDARD, OUTPUT_RINGCT);
}


UniValue sendblindtopart(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 6)
        throw std::runtime_error(SendHelp(OUTPUT_CT, OUTPUT_STANDARD));
    
    return SendToInner(request, OUTPUT_CT, OUTPUT_STANDARD);
}

UniValue sendblindtoblind(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 6)
        throw std::runtime_error(SendHelp(OUTPUT_CT, OUTPUT_CT));
    
    return SendToInner(request, OUTPUT_CT, OUTPUT_CT);
}

UniValue sendblindtoanon(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 6)
        throw std::runtime_error(SendHelp(OUTPUT_CT, OUTPUT_RINGCT));
    
    return SendToInner(request, OUTPUT_CT, OUTPUT_RINGCT);
}


UniValue sendanontopart(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8)
        throw std::runtime_error(SendHelp(OUTPUT_RINGCT, OUTPUT_STANDARD));
    
    return SendToInner(request, OUTPUT_RINGCT, OUTPUT_STANDARD);
}

UniValue sendanontoblind(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8)
        throw std::runtime_error(SendHelp(OUTPUT_RINGCT, OUTPUT_CT));
    
    return SendToInner(request, OUTPUT_RINGCT, OUTPUT_CT);
}

UniValue sendanontoanon(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8)
        throw std::runtime_error(SendHelp(OUTPUT_RINGCT, OUTPUT_RINGCT));
    
    return SendToInner(request, OUTPUT_RINGCT, OUTPUT_RINGCT);
}

UniValue sendtypeto(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 3 || request.params.size() > 8)
        throw std::runtime_error(
            "sendtypeto \"typein\" \"typeout\" [{address: , amount: , narr: , subfee:},...] (\"comment\" \"comment-to\" ringsize inputs_per_sig)\n"
            "\nSend part to multiple outputs.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"typein\"          (string, required) part/blind/anon\n"
            "2. \"typeout\"         (string, required) part/blind/anon\n"
            "3. \"outputs\"         (json, required) Array of output objects\n"
            "    3.1 \"address\"    (string, required) The particl address to send to.\n"
            "    3.2 \"amount\"     (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "    3.3 \"narr\"       (string, optional) Up to 24 character narration sent with the transaction.\n"
            "    3.4 \"subfee\"     (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "4. \"comment\"         (string, optional) A comment used to store what the transaction is for. \n"
            "                            This is not part of the transaction, just kept in your wallet.\n"
            "5. \"comment_to\"      (string, optional) A comment to store the name of the person or organization \n"
            "                            to which you're sending the transaction. This is not part of the \n"
            "                            transaction, just kept in your wallet.\n"
            "6. \"ringsize\"       (int, optional) Only applies when typein is anon.\n"
            "7. \"inputs_per_sig\" (int, optional) Only applies when typein is anon.\n"
            "8. \"test_fee\"       (bool, optional, default=false) Only return the fee it would cost to send.\n"
            "\nResult:\n"
            "\"txid\"              (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("sendtypeto", "anon part \"[{\\\"address\\\":\\\"PbpVcjgYatnkKgveaeqhkeQBFwjqR7jKBR\\\",\\\"amount\\\":0.1}]\""));
    
    std::string sTypeIn = request.params[0].get_str();
    std::string sTypeOut = request.params[1].get_str();
    
    
    OutputTypes typeIn = WordToType(sTypeIn);
    OutputTypes typeOut = WordToType(sTypeOut);
    
    if (typeIn == OUTPUT_NULL)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown input type.");
    if (typeOut == OUTPUT_NULL)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown output type.");
    
    JSONRPCRequest req = request;
    req.params.erase(0, 2);
    
    return SendToInner(req, typeIn, typeOut);
}

UniValue debugwallet(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "debugwallet [attempt_repair]\n"
            "Detect problems in wallet.\n");
    
    bool fAttemptRepair = false;
    if (request.params.size() > 0)
    {
        std::string s = request.params[0].get_str();
        if (part::IsStringBoolPositive(s))
            fAttemptRepair = true;
    };
    
    CHDWallet *pwallet = GetHDWallet();
    EnsureWalletIsUnlocked(pwallet);
    
    size_t nUnabandonedOrphans = 0;
    size_t nCoinStakes = 0;
    size_t nAbandonedOrphans = 0;
    size_t nMapWallet = 0;
    
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        
        std::map<uint256, CWalletTx>::const_iterator it;
        for (it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it)
        {
            const uint256 &wtxid = it->first;
            const CWalletTx &wtx = it->second;
            
            nMapWallet++;
            
            if (wtx.IsCoinStake())
            {
                nCoinStakes++;
                if (wtx.GetDepthInMainChain() < 1)
                {
                    if (wtx.isAbandoned())
                    {
                        nAbandonedOrphans++;
                    } else
                    {
                        nUnabandonedOrphans++;
                        LogPrintf("Unabandoned orphaned stake: %s\n", wtxid.ToString());
                        
                        if (fAttemptRepair)
                        {
                            if (!pwallet->AbandonTransaction(wtxid))
                                LogPrintf("ERROR: %s - Orphaning stake, AbandonTransaction failed for %s\n", __func__, wtxid.ToString());
                        };
                    };
                };
            };
        };
    }
    
    LogPrintf("nUnabandonedOrphans %d\n", nUnabandonedOrphans);
    LogPrintf("nCoinStakes %d\n", nCoinStakes);
    LogPrintf("nAbandonedOrphans %d\n", nAbandonedOrphans);
    LogPrintf("nMapWallet %d\n", nMapWallet);
    
    UniValue obj(UniValue::VOBJ);
    
    obj.push_back(Pair("unabandoned_orphans", (int)nUnabandonedOrphans));
    
    return obj;
}



static const CRPCCommand commands[] =
{ //  category              name                        actor (function)           okSafeMode
  //  --------------------- ------------------------    -----------------------    ----------
    { "wallet",             "extkey",                   &extkey,                   false,  {} },
    { "wallet",             "extkeyimportmaster",       &extkeyimportmaster,       false,  {} }, // import, set as master, derive account, set default account, force users to run mnemonic new first make them copy the key
    { "wallet",             "extkeygenesisimport",      &extkeygenesisimport,      false,  {} },
    { "wallet",             "keyinfo",                  &keyinfo,                  false,  {} },
    { "wallet",             "extkeyaltversion",         &extkeyaltversion,         false,  {} },
    { "wallet",             "getnewextaddress",         &getnewextaddress,         false,  {} },
    { "wallet",             "getnewstealthaddress",     &getnewstealthaddress,     false,  {} },
    { "wallet",             "importstealthaddress",     &importstealthaddress,     false,  {} },
    { "wallet",             "liststealthaddresses",     &liststealthaddresses,     false,  {} },
    
    { "wallet",             "scanchain",                &scanchain,                false,  {} },
    { "wallet",             "reservebalance",           &reservebalance,           false,  {"enabled","amount"} },
    { "wallet",             "deriverangekeys",          &deriverangekeys,          false,  {} },
    { "wallet",             "clearwallettransactions",  &clearwallettransactions,  false,  {} },
    
    { "wallet",             "filtertransactions",       &filtertransactions,       false,  {"offset","count","sort_code"} },
    { "wallet",             "filteraddresses",          &filteraddresses,          false,  {"offset","count","sort_code"} },
    { "wallet",             "manageaddressbook",        &manageaddressbook,        true,   {"action","address","label","purpose"} },
    
    { "governance",         "setvote",                  &setvote,                  false,  {"proposal","option","height_start","height_end"} },
    { "governance",         "votehistory",              &votehistory,              false,  {"current_only"} },
    { "governance",         "tallyvotes",               &tallyvotes,               false,  {"proposal","height_start","height_end"} },
    
    { "wallet",             "getstakinginfo",           &getstakinginfo,           true,  {} },
    
    //{ "wallet",             "gettransactionsummary",    &gettransactionsummary,    true,  {} },
    
    { "wallet",             "listunspentanon",          &listunspentanon,          true,  {} },
    { "wallet",             "listunspentblind",         &listunspentblind,         true,  {} },
    
    
    
    //sendparttopart // normal txn
    { "wallet",             "sendparttoblind",          &sendparttoblind,          false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration"} },
    { "wallet",             "sendparttoanon",           &sendparttoanon,           false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration"} },
    
    { "wallet",             "sendblindtopart",          &sendblindtopart,          false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration"} },
    { "wallet",             "sendblindtoblind",         &sendblindtoblind,         false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration"} },
    { "wallet",             "sendblindtoanon",          &sendblindtoanon,          false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration"} },
    
    { "wallet",             "sendanontopart",           &sendanontopart,           false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration", "ring_size", "inputs_per_sig"} },
    { "wallet",             "sendanontoblind",          &sendanontoblind,          false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration", "ring_size", "inputs_per_sig"} },
    { "wallet",             "sendanontoanon",           &sendanontoanon,           false,  {"address","amount","comment","comment_to","subtractfeefromamount", "narration", "ring_size", "inputs_per_sig"} },
    
    { "wallet",             "sendtypeto",               &sendtypeto,               false,  {"typein", "typeout", "outputs","comment","comment_to", "ring_size", "inputs_per_sig", "test_fee"} },
    
    
    
    { "wallet",             "debugwallet",              &debugwallet,               false,  {"attempt_repair"} },
    
};

void RegisterHDWalletRPCCommands(CRPCTable &t)
{
    if (GetBoolArg("-disablewallet", false))
        return;

    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
