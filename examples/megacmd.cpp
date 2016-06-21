/**
 * @file examples/megacmd.cpp
 * @brief Sample application, interactive GNU Readline CLI
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "megacmd.h"
#include "mega.h"

#define USE_VARARGS
#define PREFER_STDARG
#include <readline/readline.h>
#include <readline/history.h>
#include <iomanip>
#include <string>


#ifdef __linux__
#include <signal.h>
#endif

using namespace mega;

void clear_display(){
    rl_forced_update_display();
}

#define CLEAN_fatal if (SimpleLogger::logCurrentLevel < logFatal) ;\
    else \
        clear_display();
#define CLEAN_err if (SimpleLogger::logCurrentLevel < logError) ;\
    else \
        clear_display();
#define CLEAN_info if (SimpleLogger::logCurrentLevel < logInfo) ;\
    else \
        clear_display();
#define CLEAN_debug if (SimpleLogger::logCurrentLevel < logDebug) ;\
    else \
        clear_display();
#define CLEAN_verbose if (SimpleLogger::logCurrentLevel < logMax) ;\
    else \
        clear_display();

//MegaClient* client;
MegaApi* api;


#include "megaapi_impl.h"
/**
 * @brief This abstract class extendes the functionality of MegaRequestListener
 * allowing a synchronous beheviour
 * A virtual method is declared and should be implemented: doOnRequestFinish
 * when onRequestFinish is called by the SDK.
 * A client for this listener may wait() until the request is finished and doOnRequestFinish is completed.
 *
 * @see MegaRequestListener
 */
class SynchronousRequestListener : public MegaRequestListener //TODO: move to somewhere else
{
    private:
        MegaSemaphore* semaphore;
    protected:
        MegaRequestListener *listener = NULL;
        MegaApi *megaApi = NULL;
        MegaRequest *megaRequest = NULL;
        MegaError *megaError = NULL;

    public:
        SynchronousRequestListener()
        {
            semaphore = new MegaSemaphore();
        }
        virtual ~SynchronousRequestListener()
        {
            delete semaphore;
            if (megaRequest) delete megaRequest;
            if (megaError) delete megaError;
        }
        virtual void doOnRequestFinish(MegaApi *api, MegaRequest *request, MegaError *error) = 0;

        void onRequestFinish(MegaApi *api, MegaRequest *request, MegaError *error)
        {
            this->megaApi = api;
            if (megaRequest) delete megaRequest; //in case of reused listener
            this->megaRequest = request->copy();
            if (megaError) delete megaError; //in case of reused listener
            this->megaError = error->copy();


            doOnRequestFinish(api,request,error);
            semaphore->release();
        }

        void wait()
        {
            semaphore->wait();
        }

        int trywait(int milliseconds)
        {
            return semaphore->timedwait(milliseconds);
        }


        MegaError *getError() const;
        MegaRequest *getRequest() const;
        MegaApi *getApi() const;
};

MegaRequest *SynchronousRequestListener::getRequest() const
{
    return megaRequest;
}

MegaApi *SynchronousRequestListener::getApi() const
{
    return megaApi;
}

MegaError *SynchronousRequestListener::getError() const
{
    return megaError;
}




class MegaCmdListener : public SynchronousRequestListener
{
public:
    MegaCmdListener(MegaApi *megaApi, MegaRequestListener *listener = NULL);
    virtual ~MegaCmdListener();

    //Request callbacks
    virtual void onRequestStart(MegaApi* api, MegaRequest *request);
    virtual void doOnRequestFinish(MegaApi* api, MegaRequest *request, MegaError* e);
    virtual void onRequestUpdate(MegaApi* api, MegaRequest *request);
    virtual void onRequestTemporaryError(MegaApi *api, MegaRequest *request, MegaError* e);

protected:
    //virtual void customEvent(QEvent * event);

    MegaRequestListener *listener;
};

class MegaCmdGlobalListener : public MegaGlobalListener
{
public:
    void onNodesUpdate(MegaApi* api, MegaNodeList *nodes);
};


void MegaCmdGlobalListener::onNodesUpdate(MegaApi *api, MegaNodeList *nodes){

    int nfolders = 0;
    int nfiles = 0;
    int rfolders = 0;
    int rfiles = 0;
    if (nodes)
    for (int i=0;i<nodes->size();i++)
    {
        MegaNode *n = nodes->get(i);
        if (n->getType() == MegaNode::TYPE_FOLDER)
        {
            if (n->isRemoved()) rfolders++;
            else nfolders++;
        }
        else if (n->getType() == MegaNode::TYPE_FILE)
        {
            if (n->isRemoved()) rfiles++;
            else nfiles++;
        }
    }

    if (nfolders) { LOG_info << nfolders << " folders " << "added or updated "; CLEAN_info; }
    if (nfiles) { LOG_info << nfiles << " files " << "added or updated "; CLEAN_info; }
    if (rfolders) { LOG_info << rfolders << " folders " << "removed"; CLEAN_info; }
    if (rfiles) { LOG_info << rfiles << " files " << "removed"; CLEAN_info; }
}

// listener for all actions with the api
MegaCmdListener* megaCmdListener;

// global listener
MegaCmdGlobalListener* megaCmdGlobalListener;

// login e-mail address
static string login;

// new account signup e-mail address and name
static string signupemail, signupname;

//// signup code being confirmed
static string signupcode;

//// signup password challenge and encrypted master key
//static byte signuppwchallenge[SymmCipher::KEYLENGTH], signupencryptedmasterkey[SymmCipher::KEYLENGTH];

// local console
Console* console;

// loading progress of lengthy API responses
int responseprogress = -1;

//static const char* accesslevels() =
//{ "read-only", "read/write", "full access" };
//map<int,string> accesslevelsmap = {
//    {MegaShare::ACCESS_UNKNOWN,"Unknown access"},
//    {MegaShare::ACCESS_READ,"read access"},
//    {MegaShare::ACCESS_READWRITE,"read/write access"},
//    {MegaShare::ACCESS_FULL,"full access"},
//    {MegaShare::ACCESS_OWNER,"owner access"},
//};
const char* getAccessLevelStr(int level){
    switch (level){
        case MegaShare::ACCESS_UNKNOWN: return "unknown access"; break;
        case MegaShare::ACCESS_READ: return "read access"; break;
        case MegaShare::ACCESS_READWRITE: return "read/write access"; break;
        case MegaShare::ACCESS_FULL: return "full access"; break;
        case MegaShare::ACCESS_OWNER: return "owner access"; break;
    };
    return "undefined";
}

//const char* errorstring(error e)
//{
//    switch (e)
//    {
//        case API_OK:
//            return "No error";
//        case API_EINTERNAL:
//            return "Internal error";
//        case API_EARGS:
//            return "Invalid argument";
//        case API_EAGAIN:
//            return "Request failed, retrying";
//        case API_ERATELIMIT:
//            return "Rate limit exceeded";
//        case API_EFAILED:
//            return "Transfer failed";
//        case API_ETOOMANY:
//            return "Too many concurrent connections or transfers";
//        case API_ERANGE:
//            return "Out of range";
//        case API_EEXPIRED:
//            return "Expired";
//        case API_ENOENT:
//            return "Not found";
//        case API_ECIRCULAR:
//            return "Circular linkage detected";
//        case API_EACCESS:
//            return "Access denied";
//        case API_EEXIST:
//            return "Already exists";
//        case API_EINCOMPLETE:
//            return "Incomplete";
//        case API_EKEY:
//            return "Invalid key/integrity check failed";
//        case API_ESID:
//            return "Bad session ID";
//        case API_EBLOCKED:
//            return "Blocked";
//        case API_EOVERQUOTA:
//            return "Over quota";
//        case API_ETEMPUNAVAIL:
//            return "Temporarily not available";
//        case API_ETOOMANYCONNECTIONS:
//            return "Connection overflow";
//        case API_EWRITE:
//            return "Write error";
//        case API_EREAD:
//            return "Read error";
//        case API_EAPPKEY:
//            return "Invalid application key";
//        default:
//            return "Unknown error";
//    }
//}

//AppFile::AppFile()
//{
//    static int nextseqno;

//    seqno = ++nextseqno;
//}

//// transfer start
//void AppFilePut::start()
//{
//}

//void AppFileGet::start()
//{
//}

//// transfer completion
//void AppFileGet::completed(Transfer*, LocalNode*)
//{
//    // (at this time, the file has already been placed in the final location)
//    delete this;
//}

//void AppFilePut::completed(Transfer* t, LocalNode*)
//{
//    // perform standard completion (place node in user filesystem etc.)
//    File::completed(t, NULL);

//    delete this;
//}

//AppFileGet::~AppFileGet()
//{
//    appxferq[GET].erase(appxfer_it);
//}

//AppFilePut::~AppFilePut()
//{
//    appxferq[PUT].erase(appxfer_it);
//}

//void AppFilePut::displayname(string* dname)
//{
//    *dname = localname;
//    transfer->client->fsaccess->local2name(dname);
//}

//// transfer progress callback
//void AppFile::progress()
//{
//}

static void displaytransferdetails(Transfer* t, const char* action)
{
    string name;

    for (file_list::iterator it = t->files.begin(); it != t->files.end(); it++)
    {
        if (it != t->files.begin())
        {
            cout << "/";
        }

        (*it)->displayname(&name);
        cout << name;
    }

    cout << ": " << (t->type == GET ? "Incoming" : "Outgoing") << " file transfer " << action;
}


/*
// a new transfer was added
void DemoApp::transfer_added(Transfer* t)
{
}

// a queued transfer was removed
void DemoApp::transfer_removed(Transfer* t)
{
    displaytransferdetails(t, "removed\n");
}

void DemoApp::transfer_update(Transfer* t)
{
    // (this is handled in the prompt logic)
}

void DemoApp::transfer_failed(Transfer* t, error e)
{
    displaytransferdetails(t, "failed (");
    cout << errorstring(e) << ")" << endl;
}

void DemoApp::transfer_limit(Transfer *t)
{
    displaytransferdetails(t, "bandwidth limit reached\n");
}

void DemoApp::transfer_complete(Transfer* t)
{
    displaytransferdetails(t, "completed, ");

    if (t->slot)
    {
        cout << t->slot->progressreported * 10 / (1024 * (Waiter::ds - t->slot->starttime + 1)) << " KB/s" << endl;
    }
    else
    {
        cout << "delayed" << endl;
    }
}

// transfer about to start - make final preparations (determine localfilename, create thumbnail for image upload)
void DemoApp::transfer_prepare(Transfer* t)
{
    displaytransferdetails(t, "starting\n");

    if (t->type == GET)
    {
        // only set localfilename if the engine has not already done so
        if (!t->localfilename.size())
        {
            client->fsaccess->tmpnamelocal(&t->localfilename);
        }
    }
}

#ifdef ENABLE_SYNC
static void syncstat(Sync* sync)
{
    cout << ", local data in this sync: " << sync->localbytes << " byte(s) in " << sync->localnodes[FILENODE]
         << " file(s) and " << sync->localnodes[FOLDERNODE] << " folder(s)" << endl;
}

void DemoApp::syncupdate_state(Sync*, syncstate_t newstate)
{
    switch (newstate)
    {
        case SYNC_ACTIVE:
            cout << "Sync is now active" << endl;
            break;

        case SYNC_FAILED:
            cout << "Sync failed." << endl;

        default:
            ;
    }
}

void DemoApp::syncupdate_scanning(bool active)
{
    if (active)
    {
        cout << "Sync - scanning files and folders" << endl;
    }
    else
    {
        cout << "Sync - scan completed" << endl;
    }
}

// sync update callbacks are for informational purposes only and must not change or delete the sync itself
void DemoApp::syncupdate_local_folder_addition(Sync* sync, LocalNode *, const char* path)
{
    cout << "Sync - local folder addition detected: " << path;
    syncstat(sync);
}

void DemoApp::syncupdate_local_folder_deletion(Sync* sync, LocalNode *localNode)
{
    cout << "Sync - local folder deletion detected: " << localNode->name;
    syncstat(sync);
}

void DemoApp::syncupdate_local_file_addition(Sync* sync, LocalNode *, const char* path)
{
    cout << "Sync - local file addition detected: " << path;
    syncstat(sync);
}

void DemoApp::syncupdate_local_file_deletion(Sync* sync, LocalNode *localNode)
{
    cout << "Sync - local file deletion detected: " << localNode->name;
    syncstat(sync);
}

void DemoApp::syncupdate_local_file_change(Sync* sync, LocalNode *, const char* path)
{
    cout << "Sync - local file change detected: " << path;
    syncstat(sync);
}

void DemoApp::syncupdate_local_move(Sync*, LocalNode *localNode, const char* path)
{
    cout << "Sync - local rename/move " << localNode->name << " -> " << path << endl;
}

void DemoApp::syncupdate_local_lockretry(bool locked)
{
    if (locked)
    {
        cout << "Sync - waiting for local filesystem lock" << endl;
    }
    else
    {
        cout << "Sync - local filesystem lock issue resolved, continuing..." << endl;
    }
}

void DemoApp::syncupdate_remote_move(Sync *, Node *n, Node *prevparent)
{
    cout << "Sync - remote move " << n->displayname() << ": " << (prevparent ? prevparent->displayname() : "?") <<
            " -> " << (n->parent ? n->parent->displayname() : "?") << endl;
}

void DemoApp::syncupdate_remote_rename(Sync *, Node *n, const char *prevname)
{
    cout << "Sync - remote rename " << prevname << " -> " <<  n->displayname() << endl;
}

void DemoApp::syncupdate_remote_folder_addition(Sync *, Node* n)
{
    cout << "Sync - remote folder addition detected " << n->displayname() << endl;
}

void DemoApp::syncupdate_remote_file_addition(Sync *, Node* n)
{
    cout << "Sync - remote file addition detected " << n->displayname() << endl;
}

void DemoApp::syncupdate_remote_folder_deletion(Sync *, Node* n)
{
    cout << "Sync - remote folder deletion detected " << n->displayname() << endl;
}

void DemoApp::syncupdate_remote_file_deletion(Sync *, Node* n)
{
    cout << "Sync - remote file deletion detected " << n->displayname() << endl;
}

void DemoApp::syncupdate_get(Sync*, Node *, const char* path)
{
    cout << "Sync - requesting file " << path << endl;
}

void DemoApp::syncupdate_put(Sync*, LocalNode *, const char* path)
{
    cout << "Sync - sending file " << path << endl;
}

void DemoApp::syncupdate_remote_copy(Sync*, const char* name)
{
    cout << "Sync - creating remote file " << name << " by copying existing remote file" << endl;
}

static const char* treestatename(treestate_t ts)
{
    switch (ts)
    {
        case TREESTATE_NONE:
            return "None/Undefined";
        case TREESTATE_SYNCED:
            return "Synced";
        case TREESTATE_PENDING:
            return "Pending";
        case TREESTATE_SYNCING:
            return "Syncing";
    }

    return "UNKNOWN";
}

void DemoApp::syncupdate_treestate(LocalNode* l)
{
    cout << "Sync - state change of node " << l->name << " to " << treestatename(l->ts) << endl;
}

// generic name filter
// FIXME: configurable regexps
static bool is_syncable(const char* name)
{
    return *name != '.' && *name != '~' && strcmp(name, "Thumbs.db") && strcmp(name, "desktop.ini");
}

// determines whether remote node should be synced
bool DemoApp::sync_syncable(Node* n)
{
    return is_syncable(n->displayname());
}

// determines whether local file should be synced
bool DemoApp::sync_syncable(const char* name, string* localpath, string* localname)
{
    return is_syncable(name);
}
#endif

//AppFileGet::AppFileGet(Node* n, handle ch, byte* cfilekey, m_off_t csize, m_time_t cmtime, string* cfilename,
//                       string* cfingerprint)
//{
//    if (n)
//    {
//        h = n->nodehandle;
//        hprivate = true;

//        *(FileFingerprint*) this = *n;
//        name = n->displayname();
//    }
//    else
//    {
//        h = ch;
//        memcpy(filekey, cfilekey, sizeof filekey);
//        hprivate = false;

//        size = csize;
//        mtime = cmtime;

//        if (!cfingerprint->size() || !unserializefingerprint(cfingerprint))
//        {
//            memcpy(crc, filekey, sizeof crc);
//        }

//        name = *cfilename;
//    }

//    localname = name;
//    client->fsaccess->name2local(&localname);
//}

//AppFilePut::AppFilePut(string* clocalname, handle ch, const char* ctargetuser)
//{
//    // this assumes that the local OS uses an ASCII path separator, which should be true for most
//    string separator = client->fsaccess->localseparator;

//    // full local path
//    localname = *clocalname;

//    // target parent node
//    h = ch;

//    // target user
//    targetuser = ctargetuser;

//    // erase path component
//    name = *clocalname;
//    client->fsaccess->local2name(&name);
//    client->fsaccess->local2name(&separator);

//    name.erase(0, name.find_last_of(*separator.c_str()) + 1);
//}

// user addition/update (users never get deleted)
void DemoApp::users_updated(User** u, int count)
{
    if (count == 1)
    {
        cout << "1 user received or updated" << endl;
    }
    else
    {
        cout << count << " users received or updated" << endl;
    }
}

#ifdef ENABLE_CHAT

void DemoApp::chatcreate_result(TextChat *chat, error e)
{
    if (e)
    {
        cout << "Chat creation failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Chat created successfully" << endl;
        printChatInformation(chat);
        cout << endl;
    }
}

void DemoApp::chatfetch_result(textchat_vector *chats, error e)
{
    if (e)
    {
        cout << "Chat fetching failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        if (chats->size() == 1)
        {
            cout << "1 chat received or updated" << endl;
        }
        else
        {
            cout << chats->size() << " chats received or updated" << endl;
        }

        for (textchat_vector::iterator it = chats->begin(); it < chats->end(); it++)
        {
            printChatInformation(*it);
            cout << endl;
        }
    }
}

void DemoApp::chatinvite_result(error e)
{
    if (e)
    {
        cout << "Chat invitation failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Chat invitation successful" << endl;
    }
}

void DemoApp::chatremove_result(error e)
{
    if (e)
    {
        cout << "Peer removal failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Peer removal successful" << endl;
    }
}

void DemoApp::chaturl_result(string *url, error e)
{
    if (e)
    {
        cout << "Chat URL retrieval failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Chat URL: " << *url << endl;
    }

}

void DemoApp::chatgrantaccess_result(error e)
{
    if (e)
    {
        cout << "Grant access to node failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Access to node granted successfully" << endl;
    }
}

void DemoApp::chatremoveaccess_result(error e)
{
    if (e)
    {
        cout << "Revoke access to node failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Access to node removed successfully" << endl;
    }
}

void DemoApp::chats_updated(textchat_vector *chats)
{
    if (chats)
    {
        if (chats->size() == 1)
        {
            cout << "1 chat updated or created" << endl;
        }
        else
        {
            cout << chats->size() << " chats updated or created" << endl;
        }
    }
}

void DemoApp::printChatInformation(TextChat *chat)
{
    if (!chat)
    {
        return;
    }

    char hstr[sizeof(handle) * 4 / 3 + 4];
    Base64::btoa((const byte *)&chat->id, sizeof(handle), hstr);

    cout << "Chat ID: " << hstr << endl;
    cout << "\tOwn privilege level: " << getPrivilegeString(chat->priv) << endl;
    cout << "\tChat shard: " << chat->shard << endl;
    cout << "\tURL: " << chat->url << endl;
    if (chat->group)
    {
        cout << "\tGroup chat: yes" << endl;
    }
    else
    {
        cout << "\tGroup chat: no" << endl;
    }
    cout << "\tPeers:";

    if (chat->userpriv)
    {
        cout << "\t\t(userhandle)\t(privilege level)" << endl;
        for (unsigned i = 0; i < chat->userpriv->size(); i++)
        {
            Base64::btoa((const byte *)&chat->userpriv->at(i).first, sizeof(handle), hstr);
            cout << "\t\t\t" << hstr;
            cout << "\t" << getPrivilegeString(chat->userpriv->at(i).second) << endl;
        }
    }
    else
    {
        cout << " no peers (only you as participant)" << endl;
    }
}

string DemoApp::getPrivilegeString(privilege_t priv)
{
    switch (priv)
    {
    case PRIV_FULL:
        return "PRIV_FULL (full access)";
    case PRIV_OPERATOR:
        return "PRIV_OPERATOR (operator)";
    case PRIV_RO:
        return "PRIV_RO (read-only)";
    case PRIV_RW:
        return "PRIV_RW (read-write)";
    case PRIV_RM:
        return "PRIV_RM (removed)";
    case PRIV_UNKNOWN:
    default:
        return "PRIV_UNKNOWN";
    }
}

#endif


void DemoApp::pcrs_updated(PendingContactRequest** list, int count)
{
    int deletecount = 0;
    int updatecount = 0;
    if (list != NULL)
    {
        for (int i = 0; i < count; i++)
        {
            if (list[i]->changed.deleted)
            {
                deletecount++;
            }
            else
            {
                updatecount++;
            }
        }
    }
    else
    {
        // All pcrs are updated
        for (handlepcr_map::iterator it = client->pcrindex.begin(); it != client->pcrindex.end(); it++)
        {
            if (it->second->changed.deleted)
            {
                deletecount++;
            }
            else
            {
                updatecount++;
            }
        }
    }

    if (deletecount != 0)
    {
        cout << deletecount << " pending contact request" << (deletecount != 1 ? "s" : "") << " deleted" << endl;
    }
    if (updatecount != 0)
    {
        cout << updatecount << " pending contact request" << (updatecount != 1 ? "s" : "") << " received or updated" << endl;
    }
}

void DemoApp::setattr_result(handle, error e)
{
    if (e)
    {
        cout << "Node attribute update failed (" << errorstring(e) << ")" << endl;
    }
}

void DemoApp::rename_result(handle, error e)
{
    if (e)
    {
        cout << "Node move failed (" << errorstring(e) << ")" << endl;
    }
}

void DemoApp::unlink_result(handle, error e)
{
    if (e)
    {
        cout << "Node deletion failed (" << errorstring(e) << ")" << endl;
    }
}

void DemoApp::fetchnodes_result(error e)
{
    if (e)
    {
        cout << "File/folder retrieval failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        // check if we fetched a folder link and the key is invalid
        handle h = client->getrootpublicfolder();
        if (h != UNDEF)
        {
            Node *n = client->nodebyhandle(h);
            if (n && (n->attrs.map.find('n') == n->attrs.map.end()))
            {
                cout << "File/folder retrieval succeed, but encryption key is wrong." << endl;
            }
        }
    }
}

void DemoApp::putnodes_result(error e, targettype_t t, NewNode* nn)
{
    if (t == USER_HANDLE)
    {
        delete[] nn;

        if (!e)
        {
            cout << "Success." << endl;
        }
    }

    if (e)
    {
        cout << "Node addition failed (" << errorstring(e) << ")" << endl;
    }
}

void DemoApp::share_result(error e)
{
    if (e)
    {
        cout << "Share creation/modification request failed (" << errorstring(e) << ")" << endl;
    }
}

void DemoApp::share_result(int, error e)
{
    if (e)
    {
        cout << "Share creation/modification failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Share creation/modification succeeded" << endl;
    }
}

void DemoApp::setpcr_result(handle h, error e, opcactions_t action)
{
    if (e)
    {
        cout << "Outgoing pending contact request failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        if (h == UNDEF)
        {
            // must have been deleted
            cout << "Outgoing pending contact request " << (action == OPCA_DELETE ? "deleted" : "reminded") << " successfully" << endl;
        }
        else
        {
            char buffer[12];
            Base64::btoa((byte*)&h, sizeof(h), buffer);
            cout << "Outgoing pending contact request succeeded, id: " << buffer << endl;
        }
    }
}

void DemoApp::updatepcr_result(error e, ipcactions_t action)
{
    if (e)
    {
        cout << "Incoming pending contact request update failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        string labels[3] = {"accepted", "denied", "ignored"};
        cout << "Incoming pending contact request successfully " << labels[(int)action] << endl;
    }
}

void DemoApp::fa_complete(Node* n, fatype type, const char* data, uint32_t len)
{
    cout << "Got attribute of type " << type << " (" << len << " byte(s)) for " << n->displayname() << endl;
}

int DemoApp::fa_failed(handle, fatype type, int retries, error e)
{
    cout << "File attribute retrieval of type " << type << " failed (retries: " << retries << ") error: " << e << endl;

    return retries > 2;
}

void DemoApp::putfa_result(handle, fatype, error e)
{
    if (e)
    {
        cout << "File attribute attachment failed (" << errorstring(e) << ")" << endl;
    }
}

void DemoApp::invite_result(error e)
{
    if (e)
    {
        cout << "Invitation failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Success." << endl;
    }
}

void DemoApp::putua_result(error e)
{
    if (e)
    {
        cout << "User attribute update failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Success." << endl;
    }
}

void DemoApp::getua_result(error e)
{
    cout << "User attribute retrieval failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::getua_result(byte* data, unsigned l)
{
    cout << "Received " << l << " byte(s) of user attribute: ";
    fwrite(data, 1, l, stdout);
    cout << endl;
}

void DemoApp::notify_retry(dstime dsdelta)
{
    if (dsdelta)
    {
        cout << "API request failed, retrying in " << dsdelta * 100 << " ms - Use 'retry' to retry immediately..."
             << endl;
    }
    else
    {
        cout << "Retried API request completed" << endl;
    }
}
*/
static void store_line(char*);
static void process_line(char *);
static char* line;

static AccountDetails account;

static handle cwd = UNDEF;
static MegaNode* rootNode = NULL;
static char *session;

static const char* rootnodenames[] =
{ "ROOT", "INBOX", "RUBBISH" };
static const char* rootnodepaths[] =
{ "/", "//in", "//bin" };

static void nodestats(int* c, const char* action)
{
    if (c[FILENODE])
    {
        cout << c[FILENODE] << ((c[FILENODE] == 1) ? " file" : " files");
    }
    if (c[FILENODE] && c[FOLDERNODE])
    {
        cout << " and ";
    }
    if (c[FOLDERNODE])
    {
        cout << c[FOLDERNODE] << ((c[FOLDERNODE] == 1) ? " folder" : " folders");
    }

    if (c[FILENODE] || c[FOLDERNODE])
    {
        cout << " " << action << endl;
    }
}

// list available top-level nodes and contacts/incoming shares
static void listtrees()
{
//    //TODO: modify using API
    for (int i = 0; i < (int) (sizeof rootnodenames/sizeof *rootnodenames); i++)
    {
        cout << rootnodenames[i] << " on " << rootnodepaths[i] << endl;
    }

    MegaShareList * msl = api->getInSharesList();
    for (int i=0;i<msl->size();i++)
    {
        MegaShare *share = msl->get(i);

        cout << "INSHARE on " << share->getUser() << ":" << api->getNodeByHandle(share->getNodeHandle())->getName() << " (" << getAccessLevelStr(share->getAccess()) << ")" << endl;
        share->getUser(); //TODO: voy por aqui, que imprima lo de turno
    }

    delete (msl);
}


// returns node pointer determined by path relative to cwd
// path naming conventions:
// * path is relative to cwd
// * /path is relative to ROOT
// * //in is in INBOX
// * //bin is in RUBBISH
// * X: is user X's INBOX
// * X:SHARE is share SHARE from user X
// * : and / filename components, as well as the \, must be escaped by \.
// (correct UTF-8 encoding is assumed)
// returns NULL if path malformed or not found
static MegaNode* nodebypath(const char* ptr, string* user = NULL, string* namepart = NULL)
{
    vector<string> c;
    string s;
    int l = 0;
    const char* bptr = ptr;
    int remote = 0;
    MegaNode* n;
    MegaNode* nn;

    // split path by / or :
    do {
        if (!l)
        {
            if (*ptr >= 0)
            {
                if (*ptr == '\\')
                {
                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ++ptr;

                    if (*bptr == 0)
                    {
                        c.push_back(s);
                        break;
                    }

                    ptr++;
                    continue;
                }

                if (*ptr == '/' || *ptr == ':' || !*ptr)
                {
                    if (*ptr == ':')
                    {
                        if (c.size())
                        {
                            return NULL;
                        }

                        remote = 1;
                    }

                    if (ptr > bptr)
                    {
                        s.append(bptr, ptr - bptr);
                    }

                    bptr = ptr + 1;

                    c.push_back(s);

                    s.erase();
                }
            }
            else if ((*ptr & 0xf0) == 0xe0)
            {
                l = 1;
            }
            else if ((*ptr & 0xf8) == 0xf0)
            {
                l = 2;
            }
            else if ((*ptr & 0xfc) == 0xf8)
            {
                l = 3;
            }
            else if ((*ptr & 0xfe) == 0xfc)
            {
                l = 4;
            }
        }
        else
        {
            l--;
        }
    } while (*ptr++);

    if (l)
    {
        return NULL;
    }

    if (remote)
    {
        // target: user inbox - record username/email and return NULL
        if (c.size() == 2 && !c[1].size())
        {
            if (user)
            {
                *user = c[0];
            }

            return NULL;
        }

        //TODO: implement finding users share node.
//        User* u;
//        itll be sth like: if ((u = finduser(c[0].c_str()))) //TODO: implement findUser
//        if ((u = client->finduser(c[0].c_str())))
//        {/*
//            // locate matching share from this user
//            handle_set::iterator sit;
//            string name;
//            for (sit = u->sharing.begin(); sit != u->sharing.end(); sit++)
//            {
//                if ((n = client->nodebyhandle(*sit)))
//                {
//                    if(!name.size())
//                    {
//                        name =  c[1];
//                        n->client->fsaccess->normalize(&name);
//                    }

//                    if (!strcmp(name.c_str(), n->displayname()))
//                    {
//                        l = 2;
//                        break;
//                    }
//                }
//            }
//        }*/

        if (!l)
        {
            return NULL;
        }
    }
    else //local
    {
        // path starting with /
        if (c.size() > 1 && !c[0].size())
        {
            // path starting with //
            if (c.size() > 2 && !c[1].size())
            {
                if (c[2] == "in")
                {
                    //TODO: modify using API //TODO: test & delete comments
//                    n = client->nodebyhandle(client->rootnodes[1]);
                    n = api->getInboxNode();
                }
                else if (c[2] == "bin")
                {
                    //TODO: modify using API //TODO: test & delete comments
//                    n = client->nodebyhandle(client->rootnodes[2]);
                    n = api->getRubbishNode();
                }
                else
                {
                    return NULL;
                }

                l = 3;
            }
            else
            {
                //TODO: modify using API //TODO: test & delete comments
                n = rootNode;
//                n = client->nodebyhandle(client->rootnodes[0]);

                l = 1;
            }
        }
        else
        {
            //TODO: modify using API //TODO: test & delete comments
            n = api->getNodeByHandle(cwd);
//            n = client->nodebyhandle(cwd);
        }
    }

    // parse relative path
    while (n && l < (int)c.size())
    {
        if (c[l] != ".")
        {
            if (c[l] == "..")
            {
                n = api->getParentNode(n);
            }
            else
            {
                // locate child node (explicit ambiguity resolution: not implemented)
                if (c[l].size())
                {
                    //TODO: modify using API //TODO: test & delete comments
                    nn = api->getChildNode(n, c[l].c_str());
//                    nn = client->childnodebyname(n, c[l].c_str());

                    if (!nn) //NOT FOUND
                    {
                        // mv command target? return name part of not found
                        if (namepart && l == (int) c.size() - 1) //if this is the last part, we will pass that one, so that a mv command know the name to give the the new node
                        {
                            *namepart = c[l];
                            return n;
                        }

                        return NULL;
                    }

                    n = nn;
                }
            }
        }

        l++;
    }

    return n;
}

static void listnodeshares(MegaNode* n)
{
    MegaShareList* outShares=api->getOutShares(n);
    if(outShares)
    {
        for (int i=0;i<outShares->size();i++)
//        for (share_map::iterator it = n->outshares->begin(); it != n->outshares->end(); it++)
        {
            cout << "\t" << n->getName();

            if (outShares->get(i))
            {
                cout << ", shared with " << outShares->get(i)->getUser() << " (" << getAccessLevelStr(outShares->get(i)->getAccess()) << ")"
                     << endl;
            }
            else
            {
                cout << ", shared as exported folder link" << endl;
            }
        }
        delete outShares;
    }
}

//void TreeProcListOutShares::proc(MegaClient*, Node* n)
//{
//    listnodeshares(n);
//}

static void dumptree(MegaNode* n, int recurse, int depth = 0, const char* title = NULL)
{
    if (depth)
    {
        if (!title && !(title = n->getName()))
        {
            title = "CRYPTO_ERROR";
        }

        for (int i = depth; i--; )
        {
            cout << "\t";
        }

        cout << title << " (";

        switch (n->getType())
        {
            case MegaNode::TYPE_FILE:
                cout << n->getSize();

                const char* p;
                if ((p = strchr(n->getAttrString()->c_str(), ':')))
                {
                    cout << ", has attributes " << p + 1;
                }

                if (UNDEF != n->getPublicHandle())
                //if (n->plink)
                {
                    cout << ", shared as exported";
                    if (n->getExpirationTime()) //TODO: validate equivalence
                    //if (n->plink->ets)
                    {
                        cout << " temporal";
                    }
                    else
                    {
                        cout << " permanent";
                    }
                    cout << " file link";
                }
                break;

            case MegaNode::TYPE_FOLDER:
            {
                cout << "folder";
                MegaShareList* outShares = api->getOutShares(n);
                if (outShares)
                {
                    for (int i=0;i<outShares->size();i++)
                    {
                        if (outShares->get(i))
                        {
                            cout << ", shared with " << outShares->get(i)->getUser() << ", access "
                                 << getAccessLevelStr(outShares->get(i)->getAccess());
                        }
                    }
                    if (UNDEF != n->getPublicHandle())
                    //if (n->plink)
                    {
                        cout << ", shared as exported";
                        if (n->getExpirationTime()) //TODO: validate equivalence
//                        if (n->plink->ets)
                        {
                            cout << " temporal";
                        }
                        else
                        {
                            cout << " permanent";
                        }
                        cout << " folder link";
                    }
                    delete outShares;
                }

                MegaShareList* pendingoutShares= api->getPendingOutShares(n);
                if(pendingoutShares)
                {
                    for (int i=0;i<pendingoutShares->size();i++)
                    {
                        if (pendingoutShares->get(i))
                        {
                            cout << ", shared (still pending) with " << pendingoutShares->get(i)->getUser() << ", access "
                                 << getAccessLevelStr(pendingoutShares->get(i)->getAccess());
                        }
                    }
                    delete pendingoutShares;
                }

                if (n->isInShare())
                {
                    //cout << ", inbound " << getAccessLevelStr(n->inshare->access) << " share";
                    cout << ", inbound " << api->getAccess(n) << " share"; //TODO: validate & delete
                }
                break;
            }

            default:
                cout << "unsupported type, please upgrade";
        }
        cout << ")" << (n->isRemoved() ? " (DELETED)" : "") << endl;

        if (!recurse)
        {
            return;
        }
    }

    if (n->getType() != MegaNode::TYPE_FILE)
    {
        MegaNodeList* children= api->getChildren(n);
        if (children)
        {
            for (int i=0;i<children->size();i++)
            {
                dumptree(children->get(i), recurse, depth + 1);
            }
        delete children;
        }
    }
}


static const char * getUserInSharedNode(MegaNode *n)
{
    MegaShareList * msl = api->getInSharesList();
    for (int i=0;i<msl->size();i++)
    {
        MegaShare *share = msl->get(i);

        if (share->getNodeHandle() == n->getHandle())
        {
            delete (msl);
            return share->getUser();
        }
    }
    delete (msl);
    return NULL;
}

static void nodepath(handle h, string* path)
{
    path->clear();

    //TODO: modify using API
    if ( rootNode  && (h == rootNode->getHandle()) )
    {
        *path = "/";
        return;
    }

    MegaNode* n = api->getNodeByHandle(h);

    //TODO: modify using API
    while (n)
    {
        switch (n->getType())
        {
            case MegaNode::TYPE_FOLDER:
                path->insert(0, n->getName());

                if (n->isInShare())
                {
                    path->insert(0, ":");

                    if (const char * suser=getUserInSharedNode(n))
                    {
                        path->insert(0, suser);
                    }
                    else
                    {
                        path->insert(0, "UNKNOWN");
                    }
                    delete n;
                    return;
                }
                break;

            case MegaNode::TYPE_INCOMING:
                path->insert(0, "//in");
                delete n;
                return;

            case MegaNode::TYPE_ROOT:
                delete n;
                return;

            case MegaNode::TYPE_RUBBISH:
                path->insert(0, "//bin");
                delete n;
                return;

            case MegaNode::TYPE_UNKNOWN:
            case MegaNode::TYPE_FILE:
                path->insert(0, n->getName());
        }

        path->insert(0, "/");
        MegaNode *aux=n;
        n = api->getNodeByHandle(n->getParentHandle());
        delete aux;
    }
}

//appfile_list appxferq[2];

static char dynamicprompt[128];

static const char* prompts[] =
{
    "MEGA CMD> ", "Password:", "Old Password:", "New Password:", "Retype New Password:"
};

enum prompttype
{
    COMMAND, LOGINPASSWORD, OLDPASSWORD, NEWPASSWORD, PASSWORDCONFIRM
};

static prompttype prompt = COMMAND;

static char pw_buf[256];
static int pw_buf_pos;

static void setprompt(prompttype p)
{
    prompt = p;

    if (p == COMMAND)
    {
        console->setecho(true);
    }
    else
    {
        pw_buf_pos = 0;
        cout << prompts[p] << flush;
        console->setecho(false);
    }
}


#ifdef __linux__
void sigint_handler(int signum)
{
    rl_replace_line("", 0); //clean contents of actual command
    rl_crlf(); //move to nextline

    // reset position and print prompt
    pw_buf_pos = 0;
    cout << prompts[prompt] << flush;
}
#endif


////////////////////////////////////////
///      MegaCmdListener methods     ///
////////////////////////////////////////

void MegaCmdListener::onRequestStart(MegaApi* api, MegaRequest *request){
    if (!request)
    {
        LOG_err << " onRequestStart for undefined request "; CLEAN_err;
        return;
    }

    LOG_verbose << "onRequestStart request->getType(): " << request->getType(); CLEAN_verbose;

    switch(request->getType())
    {
        case MegaRequest::TYPE_LOGIN:
            LOG_debug << "onRequestStart login email: " << request->getEmail(); CLEAN_debug;
            break;
        default:
            LOG_debug << "onRequestStart of unregistered type of request: " << request->getType(); CLEAN_debug;
            break;
    }

    //clear_display();
}

void MegaCmdListener::doOnRequestFinish(MegaApi* api, MegaRequest *request, MegaError* e)
{
    if (!request)
    {
        LOG_err << " onRequestFinish for undefined request "; CLEAN_err;
        return;
    }

    LOG_verbose << "onRequestFinish request->getType(): " << request->getType(); CLEAN_verbose;

    switch(request->getType())
    {
//        case MegaRequest::TYPE_LOGIN:
//            LOG_debug << "onRequestFinish login email: " << request->getEmail(); CLEAN_debug;
//            if (e->getErrorCode() == MegaError::API_ENOENT) // failed to login
//            {
//                LOG_err << "onRequestFinish login failed: invalid email or password"; CLEAN_err;
//            }
//            else //login success:
//            {
//                LOG_info << "Login correct ..."; CLEAN_info;
//            }
//            break;
//        case MegaRequest::TYPE_LOGOUT:
//            LOG_debug << "onRequestFinish logout .."; CLEAN_debug;
//            if (e->getErrorCode() == MegaError::API_OK) // failed to login
//            {
//                LOG_verbose << "onRequestFinish logout ok"; CLEAN_verbose;
//            }
//            else
//            {
//                LOG_err << "onRequestFinish failed to logout"; CLEAN_err;
//            }
//        break;
//    case MegaRequest::TYPE_FETCH_NODES:
//            LOG_debug << "onRequestFinish TYPE_FETCH_NODES: "; CLEAN_debug;
//        break;
        default:
            LOG_debug << "onRequestFinish of unregistered type of request: " << request->getType(); CLEAN_debug;
            break;
    }
    //clear_display();
}

void MegaCmdListener::onRequestUpdate(MegaApi* api, MegaRequest *request){
    if (!request)
    {
        LOG_err << " onRequestUpdate for undefined request "; CLEAN_err;
        return;
    }

    LOG_verbose << "onRequestUpdate request->getType(): " << request->getType(); CLEAN_verbose;

    switch(request->getType())
    {
        case MegaRequest::TYPE_FETCH_NODES:
    {
            ostringstream s;
            if (request->getTransferredBytes()*1.0/request->getTotalBytes()*100.0 >=0){
            s << request->getTransferredBytes()*1.0/request->getTotalBytes()*100.0 << " %" ;
            }
            else{
                s<< "0 %";
            }
            rl_replace_line(s.str().c_str(), 0);rl_redisplay();

//            rl_forced_update_display(); cout <<   << "%";
        break;
    }
        default:
          LOG_debug << "onRequestUpdate of unregistered type of request: " << request->getType(); CLEAN_debug;
        break;
    }
}

void MegaCmdListener::onRequestTemporaryError(MegaApi *api, MegaRequest *request, MegaError* e){

}


MegaCmdListener::~MegaCmdListener(){

}

MegaCmdListener::MegaCmdListener(MegaApi *megaApi, MegaRequestListener *listener)
{
    this->megaApi=megaApi;
    this->listener=listener;
}




//TreeProcCopy::TreeProcCopy()
//{
//    nn = NULL;
//    nc = 0;
//}

//void TreeProcCopy::allocnodes()
//{
//    nn = new NewNode[nc];
//}

//TreeProcCopy::~TreeProcCopy()
//{
//    delete[] nn;
//}

//// determine node tree size (nn = NULL) or write node tree to new nodes array
//void TreeProcCopy::proc(MegaClient* client, Node* n)
//{
//    if (nn)
//    {
//        string attrstring;
//        SymmCipher key;
//        NewNode* t = nn + --nc;

//        // copy node
//        t->source = NEW_NODE;
//        t->type = n->type;
//        t->nodehandle = n->nodehandle;
//        t->parenthandle = n->parent->nodehandle;

//        // copy key (if file) or generate new key (if folder)
//        if (n->type == FILENODE)
//        {
//            t->nodekey = n->nodekey;
//        }
//        else
//        {
//            byte buf[FOLDERNODEKEYLENGTH];
//            PrnGen::genblock(buf, sizeof buf);
//            t->nodekey.assign((char*) buf, FOLDERNODEKEYLENGTH);
//        }

//        key.setkey((const byte*) t->nodekey.data(), n->type);

//        n->attrs.getjson(&attrstring);
//        t->attrstring = new string;
//        client->makeattr(&key, t->attrstring, attrstring.c_str());
//    }
//    else
//    {
//        nc++;
//    }
//}

int loadfile(string* name, string* data)
{
    //TODO: modify using API
//    FileAccess* fa = client->fsaccess->newfileaccess();

//    if (fa->fopen(name, 1, 0))
//    {
//        data->resize(fa->size);
//        fa->fread(data, data->size(), 0, 0);
//        delete fa;

//        return 1;
//    }

//    delete fa;

    return 0;
}

//void xferq(direction_t d, int cancel)
//{
//    string name;

//    for (appfile_list::iterator it = appxferq[d].begin(); it != appxferq[d].end(); )
//    {
//        if (cancel < 0 || cancel == (*it)->seqno)
//        {
//            (*it)->displayname(&name);

//            cout << (*it)->seqno << ": " << name;

//            if (d == PUT)
//            {
//                AppFilePut* f = (AppFilePut*) *it;

//                cout << " -> ";

//                if (f->targetuser.size())
//                {
//                    cout << f->targetuser << ":";
//                }
//                else
//                {
//                    string path;
//                    nodepath(f->h, &path);
//                    cout << path;
//                }
//            }

//            if ((*it)->transfer && (*it)->transfer->slot)
//            {
//                cout << " [ACTIVE]";
//            }
//            cout << endl;

//            if (cancel >= 0)
//            {
//                cout << "Canceling..." << endl;

//                if ((*it)->transfer)
//                {
//                    client->stopxfer(*it);
//                }
//                delete *it++;
//            }
//            else
//            {
//                it++;
//            }
//        }
//        else
//        {
//            it++;
//        }
//    }
//}

// password change-related state information
static byte pwkey[SymmCipher::KEYLENGTH];
static byte pwkeybuf[SymmCipher::KEYLENGTH];
static byte newpwkey[SymmCipher::KEYLENGTH];

// readline callback - exit if EOF, add to history unless password
static void store_line(char* l)
{
    if (!l)
    {
        delete console;
        exit(0);
    }

    if (*l && prompt == COMMAND)
    {
        add_history(l);
    }

    line = l;
}

void actUponFetchNodes(SynchronousRequestListener *srl,int timeout=-1)
{
    if (timeout==-1)
        srl->wait();
    else
    {
        int trywaitout=srl->trywait(timeout);
        if (trywaitout){
           LOG_err << "Fetch nodes took too long, it may have failed. No further actions performed"; CLEAN_err;
           return;
        }
    }

    if (srl->getError()->getErrorCode() == MegaError::API_OK)
    {
        LOG_verbose << "onRequestFinish TYPE_FETCH_NODES ok"; CLEAN_verbose;
        if (rootNode) delete rootNode;
        rootNode = srl->getApi()->getRootNode();

        MegaNode *cwdNode = (cwd==UNDEF)?NULL:api->getNodeByHandle(cwd);
        if (cwd == UNDEF || ! cwdNode)
        {
            cwd = rootNode->getHandle();
        }
        if (cwdNode) delete cwdNode;
    }
    else
    {
        LOG_err << " failed to fetch nodes. Error: " << srl->getError()->getErrorString(); CLEAN_err;
    }
}


void actUponLogin(SynchronousRequestListener *srl,int timeout=-1)
{
    if (timeout==-1)
        srl->wait();
    else
    {
        int trywaitout=srl->trywait(timeout);
        if (trywaitout){
           LOG_err << "Login took too long, it may have failed. No further actions performed"; CLEAN_err;
           return;
        }
    }

    LOG_debug << "actUponLogin login email: " << srl->getRequest()->getEmail(); CLEAN_debug;
    if (srl->getError()->getErrorCode() == MegaError::API_ENOENT) // failed to login
    {
        LOG_err << "actUponLogin login failed: invalid email or password: " << srl->getError()->getErrorString(); CLEAN_err;
    }
    else //login success:
    {
        LOG_info << "Login correct ... " << srl->getRequest()->getEmail(); CLEAN_info;

        session = srl->getApi()->dumpSession();
        srl->getApi()->fetchNodes(srl);
        actUponFetchNodes(srl,timeout);//TODO: should more accurately be max(0,timeout-timespent)
    }
}

void actUponLogout(SynchronousRequestListener *srl,int timeout=0)
{
    if (!timeout)
        srl->wait();
    else
    {
        int trywaitout=srl->trywait(timeout);
        if (trywaitout){
           LOG_err << "Logout took too long, it may have failed. No further actions performed"; CLEAN_err;
           return;
        }
    }
    if (srl->getError()->getErrorCode() == MegaError::API_OK) // failed to login
    {
        LOG_verbose << "actUponLogout logout ok"; CLEAN_verbose;
        cwd = UNDEF;
        delete rootNode;
        rootNode=NULL;
        delete session;
        session=NULL;
    }
    else
    {
        LOG_err << "actUponLogout failed to logout: " << srl->getError()->getErrorString(); CLEAN_err;
    }
}

int actUponCreateFolder(SynchronousRequestListener *srl,int timeout=0)
{
    if (!timeout)
        srl->wait();
    else
    {
        int trywaitout=srl->trywait(timeout);
        if (trywaitout){
           LOG_err << "actUponCreateFolder took too long, it may have failed. No further actions performed"; CLEAN_err;
           return 1;
        }
    }
    if (srl->getError()->getErrorCode() == MegaError::API_OK)
    {
        LOG_verbose << "actUponCreateFolder Create Folder ok"; CLEAN_verbose;
        return 0;
    }
    else
    {
        if (srl->getError()->getErrorCode() == MegaError::API_EACCESS)
        {
            LOG_err << "actUponCreateFolder failed to create folder: Access Denied"; CLEAN_err;
        }
        else
        {
            LOG_err << "actUponCreateFolder failed to create folder: " << srl->getError()->getErrorString(); CLEAN_err;
        }
        return 2;
    }
}



int actUponDeleteNode(SynchronousRequestListener *srl,int timeout=0)
{
    if (!timeout)
        srl->wait();
    else
    {
        int trywaitout=srl->trywait(timeout);
        if (trywaitout){
           LOG_err << "delete took too long, it may have failed. No further actions performed"; CLEAN_err;
           return 1;
        }
    }
    if (srl->getError()->getErrorCode() == MegaError::API_OK) // failed to login
    {
        LOG_verbose << "actUponDeleteNode delete ok"; CLEAN_verbose;
        return 0;
    }
    else
    {
        if (srl->getError()->getErrorCode() == MegaError::API_EACCESS)
        {
            LOG_err << "actUponDeleteNode failed to delete: Access Denied"; CLEAN_err;
        }
        else
        {
            LOG_err << "actUponDeleteNode failed to delete: " << srl->getError()->getErrorString(); CLEAN_err;
        }
        return 2;
    }
}



// execute command
static void process_line(char* l)
{
    switch (prompt)
    {
        case LOGINPASSWORD:
        {
        //TODO: modify using API
//            client->pw_key(l, pwkey);

//            if (signupcode.size())
//            {
//                // verify correctness of supplied signup password
//                SymmCipher pwcipher(pwkey);
//                pwcipher.ecb_decrypt(signuppwchallenge);

//                if (MemAccess::get<int64_t>((const char*)signuppwchallenge + 4))
//                {
//                    cout << endl << "Incorrect password, please try again." << endl;
//                }
//                else
//                {
//                    // decrypt and set master key, then proceed with the confirmation
//                    pwcipher.ecb_decrypt(signupencryptedmasterkey);
//                    //TODO: modify using API
////                    client->key.setkey(signupencryptedmasterkey);

////                    client->confirmsignuplink((const byte*) signupcode.data(), signupcode.size(),
////                                              MegaClient::stringhash64(&signupemail, &pwcipher));
//                }

//                signupcode.clear();
//            }
//            else
//            {
                //TODO: modify using API
//                client->login(login.c_str(), pwkey);
                  api->login(login.c_str(), l,megaCmdListener);
                  actUponLogin(megaCmdListener);
//            }

            setprompt(COMMAND);
            return;
        }

        case OLDPASSWORD:
        //TODO: modify using API
//            client->pw_key(l, pwkeybuf);

            if (!memcmp(pwkeybuf, pwkey, sizeof pwkey))
            {
                cout << endl;
                setprompt(NEWPASSWORD);
            }
            else
            {
                cout << endl << "Bad password, please try again" << endl;
                setprompt(COMMAND);
            }
            return;

        case NEWPASSWORD:
        //TODO: modify using API
//            client->pw_key(l, newpwkey);

            cout << endl;
            setprompt(PASSWORDCONFIRM);
            return;

        case PASSWORDCONFIRM:
        //TODO: modify using API
//            client->pw_key(l, pwkeybuf);

            if (memcmp(pwkeybuf, newpwkey, sizeof pwkey))
            {
                cout << endl << "Mismatch, please try again" << endl;
            }
            else
            {
                error e;

                if (signupemail.size())
                {
                    //TODO: modify using API
//                    client->sendsignuplink(signupemail.c_str(), signupname.c_str(), newpwkey);
                }
                else
                {
                    //TODO: modify using API
//                    if ((e = client->changepw(pwkey, newpwkey)) == API_OK)
//                    {
//                        memcpy(pwkey, newpwkey, sizeof pwkey);
//                        cout << endl << "Changing password..." << endl;
//                    }
//                    else
//                    {
//                        cout << "You must be logged in to change your password." << endl;
//                    }
                }
            }

            setprompt(COMMAND);
            signupemail.clear();
            return;

        case COMMAND:
            if (!l || !strcmp(l, "q") || !strcmp(l, "quit") || !strcmp(l, "exit"))
            {
                store_line(NULL);
            }

            vector<string> words;

            char* ptr = l;
            char* wptr;

            // split line into words with quoting and escaping
            for (;;)
            {
                // skip leading blank space
                while (*ptr > 0 && *ptr <= ' ')
                {
                    ptr++;
                }

                if (!*ptr)
                {
                    break;
                }

                // quoted arg / regular arg
                if (*ptr == '"')
                {
                    ptr++;
                    wptr = ptr;
                    words.push_back(string());

                    for (;;)
                    {
                        if (*ptr == '"' || *ptr == '\\' || !*ptr)
                        {
                            words[words.size() - 1].append(wptr, ptr - wptr);

                            if (!*ptr || *ptr++ == '"')
                            {
                                break;
                            }

                            wptr = ptr - 1;
                        }
                        else
                        {
                            ptr++;
                        }
                    }
                }
                else
                {
                    wptr = ptr;

                    while ((unsigned char) *ptr > ' ')
                    {
                        ptr++;
                    }

                    words.push_back(string(wptr, ptr - wptr));
                }
            }

            if (!words.size())
            {
                return;
            }

            MegaNode* n;

            if (words[0] == "?" || words[0] == "h" || words[0] == "help")
            {
                cout << "      login email [password]" << endl;
                cout << "      login exportedfolderurl#key" << endl;
                cout << "      login session" << endl;
                cout << "      begin [ephemeralhandle#ephemeralpw]" << endl;
                cout << "      signup [email name|confirmationlink]" << endl;
                cout << "      confirm" << endl;
                cout << "      session" << endl;
                cout << "      mount" << endl;
                cout << "      ls [-R] [remotepath]" << endl;
                cout << "      cd [remotepath]" << endl;
                cout << "      pwd" << endl;
                cout << "      lcd [localpath]" << endl;
                cout << "      import exportedfilelink#key" << endl;
                cout << "      put localpattern [dstremotepath|dstemail:]" << endl;
                cout << "      putq [cancelslot]" << endl;
                cout << "      get remotepath [offset [length]]" << endl;
                cout << "      get exportedfilelink#key [offset [length]]" << endl;
                cout << "      getq [cancelslot]" << endl;
                cout << "      pause [get|put] [hard] [status]" << endl;
                cout << "      getfa type [path] [cancel]" << endl;
                cout << "      mkdir remotepath" << endl;
                cout << "      rm remotepath" << endl;
                cout << "      mv srcremotepath dstremotepath" << endl;
                cout << "      cp srcremotepath dstremotepath|dstemail:" << endl;
#ifdef ENABLE_SYNC
                cout << "      sync [localpath dstremotepath|cancelslot]" << endl;
#endif
                cout << "      export remotepath [expireTime|del]" << endl;
                cout << "      share [remotepath [dstemail [r|rw|full] [origemail]]]" << endl;
                cout << "      invite dstemail [origemail|del|rmd]" << endl;
                cout << "      ipc handle a|d|i" << endl;
                cout << "      showpcr" << endl;
                cout << "      users" << endl;
                cout << "      getua attrname [email]" << endl;
                cout << "      putua attrname [del|set string|load file]" << endl;
                cout << "      putbps [limit|auto|none]" << endl;
                cout << "      killsession [all|sessionid]" << endl;
                cout << "      whoami" << endl;
                cout << "      passwd" << endl;
                cout << "      retry" << endl;
                cout << "      recon" << endl;
                cout << "      reload" << endl;
                cout << "      logout" << endl;
                cout << "      locallogout" << endl;
                cout << "      symlink" << endl;
                cout << "      version" << endl;
                cout << "      debug" << endl;
#ifdef ENABLE_CHAT
                cout << "      chatf " << endl;
                cout << "      chatc group [email ro|rw|full|op]*" << endl;
                cout << "      chati chatid email ro|rw|full|op" << endl;
                cout << "      chatr chatid [email]" << endl;
                cout << "      chatu chatid" << endl;
                cout << "      chatga chatid nodehandle uid" << endl;
                cout << "      chatra chatid nodehandle uid" << endl;
#endif
                cout << "      quit" << endl;

                return;
            }

            switch (words[0].size()) //TODO: why this???
            {
                case 2:
                case 3:
                    if (words[0] == "ls")
                    {
                        if (!api->isLoggedIn()) { LOG_err << "Not logged in"; CLEAN_err; return;}
                        int recursive = words.size() > 1 && words[1] == "-R";

                        if ((int) words.size() > recursive + 1)
                        {
                            n = nodebypath(words[recursive + 1].c_str());
                        }
                        else
                        {
                            //TODO: modify using API
                            n = api->getNodeByHandle(cwd);
//                            n = client->nodebyhandle(cwd);
                            //TODO: save cwd somewhere?
                        }

                        if (n)
                        {
                            dumptree(n, recursive);
                            delete n;
                        }

                        return;
                    }
                    else if (words[0] == "cd")
                    {
                        if (!api->isLoggedIn()) { LOG_err << "Not logged in";CLEAN_err; return; }
                        if (words.size() > 1)
                        {
                            if ((n = nodebypath(words[1].c_str())))
                            {
                                if (n->getType() == MegaNode::TYPE_FILE)
                                {
                                    LOG_err << words[1] << ": Not a directory"; CLEAN_err;
                                }
                                else
                                {
                                    cwd = n->getHandle();
                                }
                                delete n;
                            }
                            else
                            {
                                LOG_err << words[1] << ": No such file or directory"; CLEAN_err;
                            }
                        }
                        else
                        {
                            if (!rootNode) {LOG_err << "nodes not fetched"; CLEAN_err; return; }
                              cwd = rootNode->getHandle();
                        }

                        return;
                    }
                    else if (words[0] == "rm")
                    {
                        if (words.size() > 1)
                        {
                            for (int i=1;i<words.size();i++ )
                            {
                                MegaNode * nodeToDelete = nodebypath(words[i].c_str());
                                if (nodeToDelete)
                                {
                                    LOG_verbose << "Deleting recursively: " << words[i]; CLEAN_verbose;
                                    api->remove(nodeToDelete, megaCmdListener);
                                    actUponDeleteNode(megaCmdListener);
                                    delete nodeToDelete;
                                }

                            }
                        }
                        else
                        {
                            cout << "      rm remotepath" << endl;
                        }

                        return;
                    }
                    else if (words[0] == "mv")
                    {
                        MegaNode* tn; //target node
                        string newname;

                        if (words.size() > 2)
                        {
                            // source node must exist
                            if (n = nodebypath(words[1].c_str()))
                            {

                                // we have four situations:
                                // 1. target path does not exist - fail
                                // 2. target node exists and is folder - move
                                // 3. target node exists and is file - delete and rename (unless same)
                                // 4. target path exists, but filename does not - rename
                                if ((tn = nodebypath(words[2].c_str(), NULL, &newname)))
                                {
                                    if (newname.size()) //target not found, but tn has what was before the last "/" in the path.
                                    {
                                        if (tn->getType() == MegaNode::TYPE_FILE)
                                        {
                                            cout << words[2] << ": Not a directory" << endl;

                                            return;
                                        }
                                        else //move and rename!
                                        {
                                            api->moveNode(n,tn,megaCmdListener);
                                            megaCmdListener->wait(); // TODO: act upon move. log access denied...
                                            if (megaCmdListener->getError() && megaCmdListener->getError()->getErrorCode() == MegaError::API_OK)
                                            {
                                                api->renameNode(n,newname.c_str(),megaCmdListener);
                                                megaCmdListener->wait(); // TODO: act upon rename. log access denied...
                                            }
                                            else
                                            {
                                                LOG_err << "Won't rename, since move failed " << n->getName() <<" to " << tn->getName() << " : " << megaCmdListener->getError()->getErrorCode(); CLEAN_err;
                                            }
                                        }
                                    }
                                    else //target found
                                    {
                                        if (tn->getType() == MegaNode::TYPE_FILE) //move & remove old & rename new
                                        {
                                            // (there should never be any orphaned filenodes)
                                            MegaNode *tnParentNode = api->getNodeByHandle(tn->getParentHandle());
                                            if (!tn->getParentHandle() || !tnParentNode )
                                            {
                                                return;
                                            }
                                            delete tnParentNode;

                                            //move into the parent of target node
                                            api->moveNode(n,api->getNodeByHandle(tn->getParentHandle()),megaCmdListener);
                                            megaCmdListener->wait(); //TODO: do actuponmove...

                                            const char* name_to_replace = tn->getName();

                                            //remove (replaced) target node
                                            if (n != tn) //just in case moving to same location
                                            {
                                                api->remove(tn,megaCmdListener); //remove target node
                                                megaCmdListener->wait(); //TODO: actuponremove ...
                                                if (megaCmdListener->getError() && megaCmdListener->getError()->getErrorCode() != MegaError::API_OK)
                                                {
                                                    LOG_err << "Couldnt move " << n->getName() <<" to " << tn->getName() << " : " << megaCmdListener->getError()->getErrorCode(); CLEAN_err;
                                                }
                                            }

                                            // rename moved node with the new name
                                            if (megaCmdListener->getError() && megaCmdListener->getError()->getErrorCode() == MegaError::API_OK)
                                            {
                                                if (!strcmp(name_to_replace,n->getName()))
                                                {
                                                    api->renameNode(n,name_to_replace,megaCmdListener);
                                                    megaCmdListener->wait(); // TODO: act upon rename. log access denied...
                                                }
                                            }
                                            else
                                            {
                                                LOG_err << "Won't rename, since move failed " << n->getName() <<" to " << tn->getName() << " : " << megaCmdListener->getError()->getErrorCode(); CLEAN_err;
                                            }
                                        }
                                        else // target is a folder
                                        {
//                                            e = client->checkmove(n, tn);
                                            api->moveNode(n,tn,megaCmdListener);
                                            megaCmdListener->wait();
                                            //TODO: act upon...
                                        }
                                    }
                                    if (n != tn) //just in case moving to same location
                                        delete tn;
                                }
                                else //target not found (not even its folder), cant move
                                {
                                    cout << words[2] << ": No such directory" << endl;
                                }
                                delete n;
                            }
                            else
                            {
                                cout << words[1] << ": No such file or directory" << endl;
                            }
                        }
                        else
                        {
                            cout << "      mv srcremotepath dstremotepath" << endl;
                        }

                        return;
                    }
//                    else if (words[0] == "cp")
//                    {
//                        Node* tn;
//                        string targetuser;
//                        string newname;
//                        error e;

//                        if (words.size() > 2)
//                        {
//                            if ((n = nodebypath(words[1].c_str())))
//                            {
//                                if ((tn = nodebypath(words[2].c_str(), &targetuser, &newname)))
//                                {
//                                    //TODO: modify using API
////                                    if (!client->checkaccess(tn, RDWR))
////                                    {
////                                        cout << "Write access denied" << endl;

////                                        return;
////                                    }

//                                    if (tn->type == FILENODE)
//                                    {
//                                        if (n->type == FILENODE)
//                                        {
//                                            // overwrite target if source and taret are files

//                                            // (there should never be any orphaned filenodes)
//                                            if (!tn->parent)
//                                            {
//                                                return;
//                                            }

//                                            // ...delete target...
//                                            //TODO: modify using API
////                                            e = client->unlink(tn);

//                                            if (e)
//                                            {
//                                                cout << "Cannot delete existing file (" << errorstring(e) << ")"
//                                                     << endl;
//                                            }

//                                            // ...and set target to original target's parent
//                                            tn = tn->parent;
//                                        }
//                                        else
//                                        {
//                                            cout << "Cannot overwrite file with folder" << endl;
//                                            return;
//                                        }
//                                    }
//                                }

//                                TreeProcCopy tc;
//                                unsigned nc;

//                                // determine number of nodes to be copied
//                                //TODO: modify using API
////                                client->proctree(n, &tc);

//                                tc.allocnodes();
//                                nc = tc.nc;

//                                // build new nodes array
//                                //TODO: modify using API
////                                client->proctree(n, &tc);

//                                // if specified target is a filename, use it
//                                if (newname.size())
//                                {
//                                    SymmCipher key;
//                                    string attrstring;

//                                    // copy source attributes and rename
//                                    AttrMap attrs;

//                                    attrs.map = n->attrs.map;

//                                    //TODO: modify using API
////                                    client->fsaccess->normalize(&newname);
//                                    attrs.map['n'] = newname;

//                                    key.setkey((const byte*) tc.nn->nodekey.data(), tc.nn->type);

//                                    // JSON-encode object and encrypt attribute string
//                                    attrs.getjson(&attrstring);
//                                    tc.nn->attrstring = new string;
//                                    //TODO: modify using API
////                                    client->makeattr(&key, tc.nn->attrstring, attrstring.c_str());
//                                }

//                                // tree root: no parent
//                                tc.nn->parenthandle = UNDEF;

//                                if (tn)
//                                {
//                                    // add the new nodes
//                                    //TODO: modify using API
////                                    client->putnodes(tn->nodehandle, tc.nn, nc);

//                                    // free in putnodes_result()
//                                    tc.nn = NULL;
//                                }
//                                else
//                                {
//                                    if (targetuser.size())
//                                    {
//                                        cout << "Attempting to drop into user " << targetuser << "'s inbox..." << endl;

//                                        //TODO: modify using API
////                                        client->putnodes(targetuser.c_str(), tc.nn, nc);

//                                        // free in putnodes_result()
//                                        tc.nn = NULL;
//                                    }
//                                    else
//                                    {
//                                        cout << words[2] << ": No such file or directory" << endl;
//                                    }
//                                }
//                            }
//                            else
//                            {
//                                cout << words[1] << ": No such file or directory" << endl;
//                            }
//                        }
//                        else
//                        {
//                            cout << "      cp srcremotepath dstremotepath|dstemail:" << endl;
//                        }

//                        return;
//                    }
//                    else if (words[0] == "du")
//                    {
//                        TreeProcDU du;

//                        if (words.size() > 1)
//                        {
//                            if (!(n = nodebypath(words[1].c_str())))
//                            {
//                                cout << words[1] << ": No such file or directory" << endl;

//                                return;
//                            }
//                        }
//                        else
//                        {
//                            //TODO: modify using API
////                            n = client->nodebyhandle(cwd);
//                        }

//                        if (n)
//                        {
//                            //TODO: modify using API
////                            client->proctree(n, &du);

//                            cout << "Total storage used: " << (du.numbytes / 1048576) << " MB" << endl;
//                            cout << "Total # of files: " << du.numfiles << endl;
//                            cout << "Total # of folders: " << du.numfolders << endl;
//                        }

//                        return;
//                    }
//                    break;

//                case 3:
//                    if (words[0] == "get")
//                    {
//                        if (words.size() > 1)
//                        {
//                            //TODO: modify using API
////                            if (client->openfilelink(words[1].c_str(), 0) == API_OK)
////                            {
////                                cout << "Checking link..." << endl;
////                                return;
////                            }

//                            n = nodebypath(words[1].c_str());

//                            if (n)
//                            {
//                                if (words.size() > 2)
//                                {
//                                    // read file slice
//                                    //TODO: modify using API
////                                    client->pread(n, atol(words[2].c_str()), (words.size() > 3) ? atol(words[3].c_str()) : 0, NULL);
//                                }
//                                else
//                                {
////                                    AppFile* f;

//                                    // queue specified file...
//                                    if (n->type == FILENODE)
//                                    {
//                                        //TODO: modify using API
////                                        f = new AppFileGet(n);
////                                        f->appxfer_it = appxferq[GET].insert(appxferq[GET].end(), f);
////                                        client->startxfer(GET, f);
//                                    }
//                                    else
//                                    {
//                                        // ...or all files in the specified folder (non-recursive)
//                                        for (node_list::iterator it = n->children.begin(); it != n->children.end(); it++)
//                                        {
//                                            if ((*it)->type == FILENODE)
//                                            {
//                                                //TODO: modify using API
////                                                f = new AppFileGet(*it);
////                                                f->appxfer_it = appxferq[GET].insert(appxferq[GET].end(), f);
////                                                client->startxfer(GET, f);
//                                            }
//                                        }
//                                    }
//                                }
//                            }
//                            else
//                            {
//                                cout << words[1] << ": No such file or folder" << endl;
//                            }
//                        }
//                        else
//                        {
//                            cout << "      get remotepath [offset [length]]" << endl << "      get exportedfilelink#key [offset [length]]" << endl;
//                        }

//                        return;
//                    }
//                    else if (words[0] == "put")
//                    {
//                        if (words.size() > 1)
//                        {
////                            AppFile* f;
//                            handle target = cwd;
//                            string targetuser;
//                            string newname;
//                            int total = 0;
//                            string localname;
//                            string name;
//                            nodetype_t type;

//                            if (words.size() > 2)
//                            {
//                                Node* n;

//                                if ((n = nodebypath(words[2].c_str(), &targetuser, &newname)))
//                                {
//                                    target = n->nodehandle;
//                                }
//                            }

//                            //TODO: modify using API
////                            if (client->loggedin() == NOTLOGGEDIN && !targetuser.size())
////                            {
////                                cout << "Not logged in." << endl;

////                                return;
////                            }
//                            //TODO: modify using API
////                            client->fsaccess->path2local(&words[1], &localname);

//                            //TODO: modify using API
////                            irAccess* da = client->fsaccess->newdiraccess();
////                            if (da->dopen(&localname, NULL, true))
////                            {
////                                while (da->dnext(NULL, &localname, true, &type))
////                                {
////                                    //TODO: modify using API
//////                                    client->fsaccess->local2path(&localname, &name);
////                                    cout << "Queueing " << name << "..." << endl;

////                                    if (type == FILENODE)
////                                    {
////                                        //TODO: modify using API
//////                                        f = new AppFilePut(&localname, target, targetuser.c_str());
//////                                        f->appxfer_it = appxferq[PUT].insert(appxferq[PUT].end(), f);
//////                                        client->startxfer(PUT, f);
//////                                        total++;
////                                    }
////                                }
////                            }
////
////                            delete da;

////                            cout << "Queued " << total << " file(s) for upload, " << appxferq[PUT].size()
////                                 << " file(s) in queue" << endl;
//                        }
//                        else
//                        {
//                            cout << "      put localpattern [dstremotepath|dstemail:]" << endl;
//                        }

//                        return;
//                    }
                    else if (words[0] == "pwd")
                    {
                        string path;

                        nodepath(cwd, &path);

                        cout << path << endl;

                        return;
                    }
//                    else if (words[0] == "lcd")
//                    {
//                        if (words.size() > 1)
//                        {
//                            string localpath;

//                            //TODO: modify using API
////                            client->fsaccess->path2local(&words[1], &localpath);

////                            if (!client->fsaccess->chdirlocal(&localpath))
////                            {
////                                cout << words[1] << ": Failed" << endl;
////                            }
//                        }
//                        else
//                        {
//                            cout << "      lcd [localpath]" << endl;
//                        }

//                        return;
//                    }
//                    else if (words[0] == "ipc")
//                    {
//                        // incoming pending contact action
//                        handle phandle;
//                        if (words.size() == 3 && Base64::atob(words[1].c_str(), (byte*) &phandle, sizeof phandle) == sizeof phandle)
//                        {
//                            ipcactions_t action;
//                            if (words[2] == "a")
//                            {
//                                action = IPCA_ACCEPT;
//                            }
//                            else if (words[2] == "d")
//                            {
//                                action = IPCA_DENY;
//                            }
//                            else if (words[2] == "i")
//                            {
//                                action = IPCA_IGNORE;
//                            }
//                            else
//                            {
//                                cout << "      ipc handle a|d|i" << endl;
//                                return;
//                            }

//                            //TODO: modify using API
////                            client->updatepcr(phandle, action);
//                        }
//                        else
//                        {
//                            cout << "      ipc handle a|d|i" << endl;
//                        }
//                        return;
//                    }
//                    break;

//                case 4:
//                    if (words[0] == "putq")
//                    {
//                        //TODO: modify using API
////                        xferq(PUT, words.size() > 1 ? atoi(words[1].c_str()) : -1);
//                        return;
//                    }
//                    else if (words[0] == "getq")
//                    {
//                        //TODO: modify using API
////                        xferq(GET, words.size() > 1 ? atoi(words[1].c_str()) : -1);
//                        return;
//                    }
//#ifdef ENABLE_SYNC
//                    else if (words[0] == "sync")
//                    {
//                        if (words.size() == 3)
//                        {
//                            Node* n = nodebypath(words[2].c_str());
//                            //TODO: modify using API
////                            if (client->checkaccess(n, FULL))
////                            {
////                                string localname;

////                                client->fsaccess->path2local(&words[1], &localname);

////                                if (!n)
////                                {
////                                    cout << words[2] << ": Not found." << endl;
////                                }
////                                else if (n->type == FILENODE)
////                                {
////                                    cout << words[2] << ": Remote sync root must be folder." << endl;
////                                }
////                                else
////                                {
////                                    error e = client->addsync(&localname, DEBRISFOLDER, NULL, n);

////                                    if (e)
////                                    {
////                                        cout << "Sync could not be added: " << errorstring(e) << endl;
////                                    }
////                                }
////                            }
////                            else
////                            {
////                                cout << words[2] << ": Syncing requires full access to path." << endl;
////                            }
//                        }
//                        else if (words.size() == 2)
//                        {
//                            int i = 0, cancel = atoi(words[1].c_str());

//                            //TODO: modify using API
////                            for (sync_list::iterator it = client->syncs.begin(); it != client->syncs.end(); it++)
////                            {
////                                if ((*it)->state > SYNC_CANCELED && i++ == cancel)
////                                {
////                                    client->delsync(*it);

////                                    cout << "Sync " << cancel << " deactivated and removed." << endl;
////                                    break;
////                                }
////                            }
//                        }
//                        else if (words.size() == 1)
//                        {
//                            //TODO: modify using API
////                            if (client->syncs.size())
////                            {
////                                int i = 0;
////                                string remotepath, localpath;

////                                for (sync_list::iterator it = client->syncs.begin(); it != client->syncs.end(); it++)
////                                {
////                                    if ((*it)->state > SYNC_CANCELED)
////                                    {
////                                        static const char* syncstatenames[] =
////                                        { "Initial scan, please wait", "Active", "Failed" };

////                                        if ((*it)->localroot.node)
////                                        {
////                                            nodepath((*it)->localroot.node->nodehandle, &remotepath);
////                                            client->fsaccess->local2path(&(*it)->localroot.localname, &localpath);

////                                            cout << i++ << ": " << localpath << " to " << remotepath << " - "
////                                                 << syncstatenames[(*it)->state] << ", " << (*it)->localbytes
////                                                 << " byte(s) in " << (*it)->localnodes[FILENODE] << " file(s) and "
////                                                 << (*it)->localnodes[FOLDERNODE] << " folder(s)" << endl;
////                                        }
////                                    }
////                                }
////                            }
////                            else
////                            {
////                                cout << "No syncs active at this time." << endl;
////                            }
//                        }
//                        else
//                        {
//                            cout << "      sync [localpath dstremotepath|cancelslot]" << endl;
//                        }

//                        return;
//                    }
//#endif
//                    break;

                case 5:
                    if (words[0] == "login")
                    {

                        //TODO: modify using API
                        if (!api->isLoggedIn())
                        {
                            if (words.size() > 1)
                            {
                                static string pw_key;
                                if (strchr(words[1].c_str(), '@'))
                                {
                                    // full account login
                                    if (words.size() > 2)
                                    {
                                        //TODO: validate & delete
                                        api->login(words[1].c_str(),words[2].c_str(),megaCmdListener);
                                        actUponLogin(megaCmdListener);

//                                        api->login(words[1].c_str(),words[2].c_str(),megaCmdListener);
//                                        megaCmdListener->wait(); //TODO: use a constant here
//                                        api->fetchNodes(megaCmdListener);
//                                        megaCmdListener->wait();


//                                        client->pw_key(words[2].c_str(), pwkey);
//                                        client->login(words[1].c_str(), pwkey);
//                                        cout << "Initiated login attempt..." << endl;
                                    }
                                    else
                                    {
                                        login = words[1];
                                        setprompt(LOGINPASSWORD);
                                    }
                                }
                                else
                                {
                                    const char* ptr;
                                    if ((ptr = strchr(words[1].c_str(), '#')))  // folder link indicator
                                    {
                                        //TODO: deal with all this
//                                        return client->app->login_result(client->folderaccess(words[1].c_str()));
                                    }
                                    else
                                    {
                                        byte session[64];
                                        int size;

                                        if (words[1].size() < sizeof session * 4 / 3)
                                        {
//                                            size = Base64::atob(words[1].c_str(), session, sizeof session);

                                            cout << "Resuming session..." << endl;
                                            return api->fastLogin(words[1].c_str(),megaCmdListener);//TODO: pass listener once created
                                            megaCmdListener->wait();
                                            //TODO: implement actUponFastlogin (https://ci.developers.mega.co.nz/view/SDK/job/megasdk-doc/ws/doc/api/html/classmega_1_1_mega_api.html#a074f01b631eab8e504f8cfae890e830c)
                                        }
                                    }

                                    cout << "Invalid argument. Please specify a valid e-mail address, "
                                         << "a folder link containing the folder key "
                                         << "or a valid session." << endl;
                                }
                            }
                            else
                            {
                                cout << "      login email [password]" << endl
                                     << "      login exportedfolderurl#key" << endl
                                     << "      login session" << endl;
                            }
                        }
                        else
                        {
                            cout << "Already logged in. Please log out first." << endl;
                        }

                        return;
                    }
                    else if (words[0] == "begin")
                    {
                        if (words.size() == 1)
                        {
                            cout << "Creating ephemeral session..." << endl;
                            //TODO: modify using API
//                            client->createephemeral();
                        }
                        else if (words.size() == 2)
                        {
                            handle uh;
                            byte pw[SymmCipher::KEYLENGTH];

                            if (Base64::atob(words[1].c_str(), (byte*) &uh, sizeof uh) == sizeof uh && Base64::atob(
                                    words[1].c_str() + 12, pw, sizeof pw) == sizeof pw)
                            {
                                //TODO: modify using API
//                                client->resumeephemeral(uh, pw);
                            }
                            else
                            {
                                cout << "Malformed ephemeral session identifier." << endl;
                            }
                        }
                        else
                        {
                            cout << "      begin [ephemeralhandle#ephemeralpw]" << endl;
                        }

                        return;
                    }
                    else if (words[0] == "mount")
                    {
                        listtrees();
                        return;
                    }
                    else if (words[0] == "share")
                    {
                        switch (words.size())
                        {
                            case 1:		// list all shares (incoming and outgoing)
                                {
//                                    TreeProcListOutShares listoutshares;
                                    Node* n;

                                    cout << "Shared folders:" << endl;

                                    //TODO: modify using API
//                                    for (unsigned i = 0; i < sizeof client->rootnodes / sizeof *client->rootnodes; i++)
//                                    {
//                                        if ((n = client->nodebyhandle(client->rootnodes[i])))
//                                        {
//                                            client->proctree(n, &listoutshares);
//                                        }
//                                    }

                                    //TODO: modify using API
//                                    for (user_map::iterator uit = client->users.begin();
//                                         uit != client->users.end(); uit++)
//                                    {
//                                        User* u = &uit->second;
//                                        Node* n;

//                                        if (u->show == VISIBLE && u->sharing.size())
//                                        {
//                                            cout << "From " << u->email << ":" << endl;

//                                            for (handle_set::iterator sit = u->sharing.begin();
//                                                 sit != u->sharing.end(); sit++)
//                                            {
//                                                //TODO: modify using API
////                                                if ((n = client->nodebyhandle(*sit)))
////                                                {
////                                                    cout << "\t" << n->displayname() << " ("
////                                                         << getAccessLevelStr(n->inshare->access) << ")" << endl;
////                                                }
//                                            }
//                                        }
//                                    }
                                }
                                break;

                            case 2:	    // list all outgoing shares on this path
                            case 3:	    // remove outgoing share to specified e-mail address
                            case 4:	    // add outgoing share to specified e-mail address
                            case 5:     // user specified a personal representation to appear as for the invitation
                                if ((n = nodebypath(words[1].c_str())))
                                {
                                    if (words.size() == 2)
                                    {
                                        listnodeshares(n);
                                    }
                                    else
                                    {
                                        accesslevel_t a = ACCESS_UNKNOWN;
                                        const char* personal_representation = NULL;
                                        if (words.size() > 3)
                                        {
                                            if (words[3] == "r" || words[3] == "ro")
                                            {
                                                a = RDONLY;
                                            }
                                            else if (words[3] == "rw")
                                            {
                                                a = RDWR;
                                            }
                                            else if (words[3] == "full")
                                            {
                                                a = FULL;
                                            }
                                            else
                                            {
                                                cout << "Access level must be one of r, rw or full" << endl;

                                                return;
                                            }

                                            if (words.size() > 4)
                                            {
                                                personal_representation = words[4].c_str();
                                            }
                                        }
                                        //TODO: modify using API
//                                        client->setshare(n, words[2].c_str(), a, personal_representation);
                                    }
                                }
                                else
                                {
                                    cout << words[1] << ": No such directory" << endl;
                                }

                                break;

                            default:
                                cout << "      share [remotepath [dstemail [r|rw|full] [origemail]]]" << endl;
                        }

                        return;
                    }
                    else if (words[0] == "users")
                    {
                        //TODO: modify using API
//                        for (user_map::iterator it = client->users.begin(); it != client->users.end(); it++)
//                        {
//                            if (it->second.email.size())
//                            {
//                                cout << "\t" << it->second.email;

//                                if (it->second.userhandle == client->me)
//                                {
//                                    cout << ", session user";
//                                }
//                                else if (it->second.show == VISIBLE)
//                                {
//                                    cout << ", visible";
//                                }
//                                else if (it->second.show == HIDDEN)
//                                {
//                                    cout << ", hidden";
//                                }
//                                else if (it->second.show == INACTIVE)
//                                {
//                                    cout << ", inactive";
//                                }
//                                else if (it->second.show == BLOCKED)
//                                {
//                                    cout << ", blocked";
//                                }
//                                else
//                                {
//                                    cout << ", unknown visibility (" << it->second.show << ")";
//                                }

//                                if (it->second.sharing.size())
//                                {
//                                    cout << ", sharing " << it->second.sharing.size() << " folder(s)";
//                                }

//                                if (it->second.pubk.isvalid())
//                                {
//                                    cout << ", public key cached";
//                                }

//                                cout << endl;
//                            }
//                        }

                        return;
                    }
                    else if (words[0] == "mkdir")
                    {
                        if (words.size() > 1)
                        {
                            MegaNode *currentnode=api->getNodeByHandle(cwd);
                            if (currentnode)
                            {
                                string rest = words[1];
                                while ( rest.length() )
                                {
                                    bool lastleave = false;
                                    size_t possep = rest.find_first_of("/");
                                    if (possep == string::npos )
                                    {
                                        possep = rest.length();
                                        lastleave=true;
                                    }

                                    string newfoldername=rest.substr(0,possep);
                                    if (!rest.length()) break;
                                    if (newfoldername.length())
                                    {
                                        MegaNode *existing_node = api->getChildNode(currentnode,newfoldername.c_str());
                                        if (!existing_node)
                                        {
                                            LOG_verbose << "Creating (sub)folder: " << newfoldername; CLEAN_verbose;
                                            api->createFolder(newfoldername.c_str(),currentnode,megaCmdListener);
                                            actUponCreateFolder(megaCmdListener);
                                            MegaNode *prevcurrentNode=currentnode;
                                            currentnode = api->getChildNode(currentnode,newfoldername.c_str());
                                            delete prevcurrentNode;
                                            if (!currentnode)
                                            {
                                                LOG_err << "Couldn't get node for created subfolder: " << newfoldername; CLEAN_err;
                                                break;
                                            }
                                        }
                                        else
                                        {
                                            delete currentnode;
                                            currentnode=existing_node;
                                        }

                                        if (lastleave && existing_node)
                                        {
                                            LOG_err << "Folder already exists: " << words[1]; CLEAN_err;
                                        }
                                    }

                                    //string rest = rest.substr(possep+1,rest.length()-possep-1);
                                    if (!lastleave)
                                    {
                                        rest = rest.substr(possep+1,rest.length());
                                    }
                                    else
                                    {
                                        break;
                                    }
                                }
                                delete currentnode;
                            }
                            else
                            {
                                cout << "      mkdir remotepath" << endl; //TODO: print usage specific command
                            }
                        }
                        else
                        {
                            LOG_err << "Couldn't get node for cwd handle: " << cwd; CLEAN_err;
                        }
                        return;
                    }
//                    else if (words[0] == "getfa")
//                    {
//                        if (words.size() > 1)
//                        {
//                            MegaNode* n;
//                            int cancel = words.size() > 2 && words[words.size() - 1] == "cancel";

//                            if (words.size() < 3)
//                            {
//                                //TODO: modify using API
////                                n = client->nodebyhandle(cwd);
//                            }
//                            else if (!(n = nodebypath(words[2].c_str())))
//                            {
//                                cout << words[2] << ": Path not found" << endl;
//                            }

//                            if (n)
//                            {
//                                int c = 0;
//                                fatype type;

//                                type = atoi(words[1].c_str());

//                                if (n->type == FILENODE)
//                                {
//                                    if (n->hasfileattribute(type))
//                                    {
//                                        //TODO: modify using API
////                                        client->getfa(n, type, cancel);
//                                        c++;
//                                    }
//                                }
//                                else
//                                {
//                                    for (node_list::iterator it = n->children.begin(); it != n->children.end(); it++)
//                                    {
//                                        if ((*it)->type == FILENODE && (*it)->hasfileattribute(type))
//                                        {
//                                            //TODO: modify using API
////                                            client->getfa(*it, type, cancel);
//                                            c++;
//                                        }
//                                    }
//                                }

//                                cout << (cancel ? "Canceling " : "Fetching ") << c << " file attribute(s) of type " << type << "..." << endl;
//                            }
//                        }
//                        else
//                        {
//                            cout << "      getfa type [path] [cancel]" << endl;
//                        }

//                        return;
//                    }
                    else if (words[0] == "getua")
                    {
                        User* u = NULL;

                        if (words.size() == 3)
                        {
                            // get other user's attribute
                            //TODO: modify using API
//                            if (!(u = client->finduser(words[2].c_str())))
//                            {
//                                cout << words[2] << ": Unknown user." << endl;
//                                return;
//                            }
                        }
                        else if (words.size() != 2)
                        {
                            cout << "      getua attrname [email]" << endl;
                            return;
                        }

                        if (!u)
                        {
                            // get logged in user's attribute
                            //TODO: modify using API
//                            if (!(u = client->finduser(client->me)))
//                            {
//                                cout << "Must be logged in to query own attributes." << endl;
//                                return;
//                            }
                        }

                        //TODO: modify using API
//                        client->getua(u, words[1].c_str());

                        return;
                    }
                    else if (words[0] == "putua")
                    {
                        if (words.size() == 2)
                        {
                            // delete attribute
                            //TODO: modify using API
//                            client->putua(words[1].c_str());

                            return;
                        }
                        else if (words.size() == 3)
                        {
                            if (words[2] == "del")
                            {
                                //TODO: modify using API
//                                client->putua(words[1].c_str());

                                return;
                            }
                        }
                        else if (words.size() == 4)
                        {
                            if (words[2] == "set")
                            {
                                //TODO: modify using API
//                                client->putua(words[1].c_str(), (const byte*) words[3].c_str(), words[3].size());

                                return;
                            }
                            else if (words[2] == "load")
                            {
                                string data, localpath;

                                //TODO: modify using API
//                                client->fsaccess->path2local(&words[3], &localpath);

                                if (loadfile(&localpath, &data))
                                {
                                    //TODO: modify using API
//                                    client->putua(words[1].c_str(), (const byte*) data.data(), data.size());
                                }
                                else
                                {
                                    cout << "Cannot read " << words[3] << endl;
                                }

                                return;
                            }
                        }

                        cout << "      putua attrname [del|set string|load file]" << endl;

                        return;
                    }
                    else if (words[0] == "pause")
                    {
                        bool getarg = false, putarg = false, hardarg = false, statusarg = false;

                        for (int i = words.size(); --i; )
                        {
                            if (words[i] == "get")
                            {
                                getarg = true;
                            }
                            if (words[i] == "put")
                            {
                                putarg = true;
                            }
                            if (words[i] == "hard")
                            {
                                hardarg = true;
                            }
                            if (words[i] == "status")
                            {
                                statusarg = true;
                            }
                        }

                        if (statusarg)
                        {
                            if (!hardarg && !getarg && !putarg)
                            {
                                //TODO: modify using API
//                                if (!client->xferpaused[GET] && !client->xferpaused[PUT])
//                                {
//                                    cout << "Transfers not paused at the moment.";
//                                }
//                                else
//                                {
//                                    if (client->xferpaused[GET])
//                                    {
//                                        cout << "GETs currently paused." << endl;
//                                    }
//                                    if (client->xferpaused[PUT])
//                                    {
//                                        cout << "PUTs currently paused." << endl;
//                                    }
//                                }
                            }
                            else
                            {
                                cout << "      pause [get|put] [hard] [status]" << endl;
                            }

                            return;
                        }

                        if (!getarg && !putarg)
                        {
                            getarg = true;
                            putarg = true;
                        }

                        if (getarg)
                        {
                            //TODO: modify using API
//                            client->pausexfers(GET, client->xferpaused[GET] ^= true, hardarg);
//                            if (client->xferpaused[GET])
//                            {
//                                cout << "GET transfers paused. Resume using the same command." << endl;
//                            }
//                            else
//                            {
//                                cout << "GET transfers unpaused." << endl;
//                            }
                        }

                        if (putarg)
                        {
                            //TODO: modify using API
//                            client->pausexfers(PUT, client->xferpaused[PUT] ^= true, hardarg);
//                            if (client->xferpaused[PUT])
//                            {
//                                cout << "PUT transfers paused. Resume using the same command." << endl;
//                            }
//                            else
//                            {
//                                cout << "PUT transfers unpaused." << endl;
//                            }
                        }

                        return;
                    }
                    else if (words[0] == "debug")
                    {
                        //TODO: modify using API
//                        cout << "Debug mode " << (client->toggledebug() ? "on" : "off") << endl;

                        return;
                    }
                    else if (words[0] == "retry")
                    {
                        //TODO: modify using API
//                        if (client->abortbackoff())
//                        {
//                            cout << "Retrying..." << endl;
//                        }
//                        else
//                        {
//                            cout << "No failed request pending." << endl;
//                        }

                        return;
                    }
                    else if (words[0] == "recon")
                    {
                        cout << "Closing all open network connections..." << endl;

                        //TODO: modify using API
//                        client->disconnect();

                        return;
                    }
#ifdef ENABLE_CHAT
                    else if (words[0] == "chatf")
                    {
                        //TODO: modify using API
//                        client->fetchChats();
                        return;
                    }
                    else if (words[0] == "chatc")
                    {
                        unsigned wordscount = words.size();
                        if (wordscount > 1 && ((wordscount - 2) % 2) == 0)
                        {
                            int group = atoi(words[1].c_str());
                            userpriv_vector *userpriv = new userpriv_vector;

                            unsigned numUsers = 0;
                            while ((numUsers+1)*2 + 2 <= wordscount)
                            {
                                string email = words[numUsers*2 + 2];
                                User *u = client->finduser(email.c_str(), 0);
                                if (!u)
                                {
                                    cout << "User not found: " << email << endl;
                                    delete userpriv;
                                    return;
                                }

                                string privstr = words[numUsers*2 + 2 + 1];
                                privilege_t priv;
                                if (privstr ==  "ro")
                                {
                                    priv = PRIV_RO;
                                }
                                else if (privstr == "rw")
                                {
                                    priv = PRIV_RW;
                                }
                                else if (privstr == "full")
                                {
                                    priv = PRIV_FULL;
                                }
                                else if (privstr == "op")
                                {
                                    priv = PRIV_OPERATOR;
                                }
                                else
                                {
                                    cout << "Unknown privilege for " << email << endl;
                                    delete userpriv;
                                    return;
                                }

                                userpriv->push_back(userpriv_pair(u->userhandle, priv));
                                numUsers++;
                            }

                            client->createChat(group, userpriv);
                            delete userpriv;
                            return;
                        }
                        else
                        {
                            cout << "      chatc group [email ro|rw|full|op]*" << endl;
                            return;
                        }
                    }
                    else if (words[0] == "chati")
                    {
                        if (words.size() == 4)
                        {
                            handle chatid;
                            Base64::atob(words[1].c_str(), (byte*) &chatid, sizeof chatid);

                            string email = words[2];
                            User *u = client->finduser(email.c_str(), 0);
                            if (!u)
                            {
                                cout << "User not found: " << email << endl;
                                return;
                            }

                            string privstr = words[3];
                            privilege_t priv;
                            if (privstr ==  "ro")
                            {
                                priv = PRIV_RO;
                            }
                            else if (privstr == "rw")
                            {
                                priv = PRIV_RW;
                            }
                            else if (privstr == "full")
                            {
                                priv = PRIV_FULL;
                            }
                            else if (privstr == "op")
                            {
                                priv = PRIV_OPERATOR;
                            }
                            else
                            {
                                cout << "Unknown privilege for " << email << endl;
                                return;
                            }

                            client->inviteToChat(chatid, u->uid.c_str(), priv);
                            return;
                        }
                        else
                        {
                            cout << "      chati chatid email ro|rw|full|op" << endl;
                            return;

                        }
                    }
                    else if (words[0] == "chatr")
                    {
                        if (words.size() > 1)
                        {
                            handle chatid;
                            Base64::atob(words[1].c_str(), (byte*) &chatid, sizeof chatid);

                            if (words.size() == 2)
                            {
                                client->removeFromChat(chatid);
                            }
                            else if (words.size() == 3)
                            {
                                string email = words[2];
                                User *u = client->finduser(email.c_str(), 0);
                                if (!u)
                                {
                                    cout << "User not found: " << email << endl;
                                    return;
                                }

                                client->removeFromChat(chatid, u->uid.c_str());
                                return;
                            }
                            else
                            {
                                cout << "      chatr chatid [email]" << endl;
                                return;
                            }
                        }
                        else
                        {
                            cout << "      chatr chatid [email]" << endl;
                            return;
                        }

                    }
                    else if (words[0] == "chatu")
                    {
                        if (words.size() == 2)
                        {
                            handle chatid;
                            Base64::atob(words[1].c_str(), (byte*) &chatid, sizeof chatid);

                            client->getUrlChat(chatid);
                            return;
                        }
                        else
                        {
                            cout << "      chatu chatid" << endl;
                            return;
                        }
                    }
#endif
                    break;

                case 6:
                    if (words[0] == "passwd")
                    {
                        //TODO: modify using API
//                        if (client->loggedin() != NOTLOGGEDIN)
//                        {
//                            setprompt(OLDPASSWORD);
//                        }
//                        else
//                        {
//                            cout << "Not logged in." << endl;
//                        }

                        return;
                    }
                    else if (words[0] == "putbps")
                    {
                        if (words.size() > 1)
                        {
                            if (words[1] == "auto")
                            {
                                //TODO: modify using API
//                                client->putmbpscap = -1;
                            }
                            else if (words[1] == "none")
                            {
                                //TODO: modify using API
//                                client->putmbpscap = 0;
                            }
                            else
                            {
                                int t = atoi(words[1].c_str());

                                if (t > 0)
                                {
                                    //TODO: modify using API
//                                    client->putmbpscap = t;
                                }
                                else
                                {
                                    cout << "      putbps [limit|auto|none]" << endl;
                                    return;
                                }
                            }
                        }

                        cout << "Upload speed limit set to ";

                        //TODO: modify using API
//                        if (client->putmbpscap < 0)
//                        {
//                            cout << "AUTO (approx. 90% of your available bandwidth)" << endl;
//                        }
//                        else if (!client->putmbpscap)
//                        {
//                            cout << "NONE" << endl;
//                        }
//                        else
//                        {
//                            cout << client->putmbpscap << " byte(s)/second" << endl;
//                        }

                        return;
                    }
                    else if (words[0] == "invite")
                    {
                        //TODO: modify using API
//                        if (client->finduser(client->me)->email.compare(words[1]))
//                        {
//                            int del = words.size() == 3 && words[2] == "del";
//                            int rmd = words.size() == 3 && words[2] == "rmd";
//                            if (words.size() == 2 || words.size() == 3)
//                            {
//                                if (del || rmd)
//                                {
//                                    client->setpcr(words[1].c_str(), del ? OPCA_DELETE : OPCA_REMIND);
//                                }
//                                else
//                                {
//                                    // Original email is not required, but can be used if this account has multiple email addresses associated,
//                                    // to have the invite come from a specific email
//                                    client->setpcr(words[1].c_str(), OPCA_ADD, "Invite from MEGAcli", words.size() == 3 ? words[2].c_str() : NULL);
//                                }
//                            }
//                            else
//                            {
//                                cout << "      invite dstemail [origemail|del|rmd]" << endl;
//                            }
//                        }
//                        else
//                        {
//                            cout << "Cannot send invitation to your own user" << endl;
//                        }

                        return;
                    }
                    else if (words[0] == "signup")
                    {
                        if (words.size() == 2)
                        {
                            const char* ptr = words[1].c_str();
                            const char* tptr;

                            if ((tptr = strstr(ptr, "#confirm")))
                            {
                                ptr = tptr + 8;
                            }

                            unsigned len = (words[1].size() - (ptr - words[1].c_str())) * 3 / 4 + 4;

                            byte* c = new byte[len];
                            len = Base64::atob(ptr, c, len);
                            // we first just query the supplied signup link,
                            // then collect and verify the password,
                            // then confirm the account
                            //TODO: modify using API
//                            client->querysignuplink(c, len);
                            delete[] c;
                        }
                        else if (words.size() == 3)
                        {
                            //TODO: modify using API
//                            switch (client->loggedin())
//                            {
//                                case FULLACCOUNT:
//                                    cout << "Already logged in." << endl;
//                                    break;

//                                case CONFIRMEDACCOUNT:
//                                    cout << "Current account already confirmed." << endl;
//                                    break;

//                                case EPHEMERALACCOUNT:
//                                    if (words[1].find('@') + 1 && words[1].find('.') + 1)
//                                    {
//                                        signupemail = words[1];
//                                        signupname = words[2];

//                                        cout << endl;
//                                        setprompt(NEWPASSWORD);
//                                    }
//                                    else
//                                    {
//                                        cout << "Please enter a valid e-mail address." << endl;
//                                    }
//                                    break;

//                                case NOTLOGGEDIN:
//                                    cout << "Please use the begin command to commence or resume the ephemeral session to be upgraded." << endl;
//                            }
                        }

                        return;
                    }
                    else if (words[0] == "whoami")
                    {
                        MegaUser *u = api->getMyUser();
                        if (u)
                        {
                            cout << "Account e-mail: " << u->getEmail() << endl;
                            // api->getAccountDetails();//TODO: continue this.

                        }
                        else
                        {
                            cout << "Not logged in." << endl;
                        }


                        //TODO: modify using API
//                        if (client->loggedin() == NOTLOGGEDIN)
//                        {
//                            cout << "Not logged in." << endl;
//                        }
//                        else
//                        {
//                            User* u;

//                            if ((u = client->finduser(client->me)))
//                            {
//                                cout << "Account e-mail: " << u->email << endl;
//                            }

//                            cout << "Retrieving account status..." << endl;

//                            client->getaccountdetails(&account, true, true, true, true, true, true);
//                        }

                        return;
                    }
//                    else if (words[0] == "export")
//                    {
//                        if (words.size() > 1)
//                        {
//                            Node* n;
//                            int del = 0;
//                            int ets = 0;

//                            if ((n = nodebypath(words[1].c_str())))
//                            {
//                                if (words.size() > 2)
//                                {
//                                    del = (words[2] == "del");
//                                    if (!del)
//                                    {
//                                        ets = atol(words[2].c_str());
//                                    }
//                                }

//                                cout << "Exporting..." << endl;

//                                error e;
//                                //TODO: modify using API
////                                if ((e = client->exportnode(n, del, ets)))
////                                {
////                                    cout << words[1] << ": Export rejected (" << errorstring(e) << ")" << endl;
////                                }
//                            }
//                            else
//                            {
//                                cout << words[1] << ": Not found" << endl;
//                            }
//                        }
//                        else
//                        {
//                            cout << "      export remotepath [expireTime|del]" << endl;
//                        }

//                        return;
//                    }
                    else if (words[0] == "import")
                    {
                        if (words.size() > 1)
                        {
                            //TODO: modify using API
//                            if (client->openfilelink(words[1].c_str(), 1) == API_OK)
//                            {
//                                cout << "Opening link..." << endl;
//                            }
//                            else
//                            {
//                                cout << "Malformed link. Format: Exported URL or fileid#filekey" << endl;
//                            }
                        }
                        else
                        {
                            cout << "      import exportedfilelink#key" << endl;
                        }

                        return;
                    }
                    else if (words[0] == "reload")
                    {
                        cout << "Reloading account..." << endl;
                        api->fetchNodes(megaCmdListener);
                        actUponFetchNodes(megaCmdListener);
                        return;
                    }
                    else if (words[0] == "logout")
                    {
                        cout << "Logging off..." << endl;
                        api->logout(megaCmdListener);
                        actUponLogout(megaCmdListener);
                        return;
                    }
#ifdef ENABLE_CHAT
                    else if (words[0] == "chatga")
                    {
                        if (words.size() == 4)
                        {
                            handle chatid;
                            Base64::atob(words[1].c_str(), (byte*) &chatid, sizeof chatid);

                            handle nodehandle;
                            Base64::atob(words[2].c_str(), (byte*) &nodehandle, sizeof nodehandle);

                            const char *uid = words[3].c_str();

                            client->grantAccessInChat(chatid, nodehandle, uid);
                            return;
                        }
                        else
                        {
                            cout << "       chatga chatid nodehandle uid" << endl;
                            return;
                        }

                    }
                    else if (words[0] == "chatra")
                    {
                        if (words.size() == 4)
                        {
                            handle chatid;
                            Base64::atob(words[1].c_str(), (byte*) &chatid, sizeof chatid);

                            handle nodehandle;
                            Base64::atob(words[2].c_str(), (byte*) &nodehandle, sizeof nodehandle);

                            const char *uid = words[3].c_str();

                            client->removeAccessInChat(chatid, nodehandle, uid);
                            return;
                        }
                        else
                        {
                            cout << "       chatra chatid nodehandle uid" << endl;
                            return;
                        }
                    }
#endif
                    break;

                case 7:
                    if (words[0] == "confirm")
                    {
                        if (signupemail.size() && signupcode.size())
                        {
                            cout << "Please type " << signupemail << "'s password to confirm the signup." << endl;
                            setprompt(LOGINPASSWORD);
                        }
                        else
                        {
                            cout << "No signup confirmation pending." << endl;
                        }

                        return;
                    }
                    else if (words[0] == "session")
                    {
                        if (api->dumpSession())
                        {
                            cout << "Your (secret) session is: " << api->dumpSession() << endl;
                        }
                        else
                        {
                            cout << "Not logged in." << endl;
                        }
                        return;
                    }
                    else if (words[0] == "symlink")
                    {
                        //TODO: modify using API
//                        if (client->followsymlinks ^= true)
//                        {
//                            cout << "Now following symlinks. Please ensure that sync does not see any filesystem item twice!" << endl;
//                        }
//                        else
//                        {
//                            cout << "No longer following symlinks." << endl;
//                        }

                        return;
                    }
                    else if (words[0] == "version")
                    {
                        cout << "MEGA SDK version: " << MEGA_MAJOR_VERSION << "." << MEGA_MINOR_VERSION << "." << MEGA_MICRO_VERSION << endl;

                        cout << "Features enabled:" << endl;

#ifdef USE_CRYPTOPP
                        cout << "* CryptoPP" << endl;
#endif

#ifdef USE_SQLITE
                        cout << "* SQLite" << endl;
#endif

#ifdef USE_BDB
                        cout << "* Berkeley DB" << endl;
#endif

#ifdef USE_INOTIFY
                        cout << "* inotify" << endl;
#endif

#ifdef HAVE_FDOPENDIR
                        cout << "* fdopendir" << endl;
#endif

#ifdef HAVE_SENDFILE
                        cout << "* sendfile" << endl;
#endif

#ifdef _LARGE_FILES
                        cout << "* _LARGE_FILES" << endl;
#endif

#ifdef USE_FREEIMAGE
                        cout << "* FreeImage" << endl;
#endif

#ifdef ENABLE_SYNC
                        cout << "* sync subsystem" << endl;
#endif


                        cwd = UNDEF;

                        return;
                    }
                    else if (words[0] == "showpcr")
                    {
                        string outgoing = "";
                        string incoming = "";
                        //TODO: modify using API
//                        for (handlepcr_map::iterator it = client->pcrindex.begin(); it != client->pcrindex.end(); it++)
//                        {
//                            if (it->second->isoutgoing)
//                            {
//                                ostringstream os;
//                                os << setw(34) << it->second->targetemail;

//                                char buffer[12];
//                                int size = Base64::btoa((byte*)&(it->second->id), sizeof(it->second->id), buffer);
//                                os << "\t(id: ";
//                                os << buffer;

//                                os << ", ts: ";

//                                os << it->second->ts;

//                                outgoing.append(os.str());
//                                outgoing.append(")\n");
//                            }
//                            else
//                            {
//                                ostringstream os;
//                                os << setw(34) << it->second->originatoremail;

//                                char buffer[12];
//                                int size = Base64::btoa((byte*)&(it->second->id), sizeof(it->second->id), buffer);
//                                os << "\t(id: ";
//                                os << buffer;

//                                os << ", ts: ";

//                                os << it->second->ts;

//                                incoming.append(os.str());
//                                incoming.append(")\n");
//                            }
//                        }
                        cout << "Incoming PCRs:" << endl << incoming << endl;
                        cout << "Outgoing PCRs:" << endl << outgoing << endl;
                        return;
                    }
                    break;

                case 11:
                    if (words[0] == "killsession")
                    {
                        if (words.size() == 2)
                        {
                            if (words[1] == "all")
                            {
                                // Kill all sessions (except current)
                                //TODO: modify using API
//                                client->killallsessions();
                            }
                            else
                            {
                                handle sessionid;
                                if (Base64::atob(words[1].c_str(), (byte*) &sessionid, sizeof sessionid) == sizeof sessionid)
                                {
                                    //TODO: modify using API
//                                    client->killsession(sessionid);
                                }
                                else
                                {
                                    cout << "invalid session id provided" << endl;
                                }
                            }
                        }
                        else
                        {
                            cout << "      killsession [all|sessionid] " << endl;
                        }
                        return;
                    }
                    else if (words[0] == "locallogout")
                    {
                        cout << "Logging off locally..." << endl;

                        cwd = UNDEF;
                        //TODO: modify using API
//                        client->locallogout();

                        return;
                    }
                    break;
            }

            cout << "?Invalid command" << endl;
    }
}

/*
// callback for non-EAGAIN request-level errors
// in most cases, retrying is futile, so the application exits
// this can occur e.g. with syntactically malformed requests (due to a bug), an invalid application key
void DemoApp::request_error(error e)
{
    if ((e == API_ESID) || (e == API_ENOENT))   // Invalid session or Invalid folder handle
    {
        cout << "Invalid or expired session, logging out..." << endl;
        //TODO: modify using API
//        client->locallogout();
        return;
    }

    cout << "FATAL: Request failed (" << errorstring(e) << "), exiting" << endl;

    delete console;
    exit(0);
}

void DemoApp::request_response_progress(m_off_t current, m_off_t total)
{
    if (total > 0)
    {
        responseprogress = current * 100 / total;
    }
    else
    {
        responseprogress = -1;
    }
}

// login result
void DemoApp::login_result(error e)
{
    if (e)
    {
        cout << "Login failed: " << errorstring(e) << endl;
    }
    else
    {
        cout << "Login successful, retrieving account..." << endl;
        //TODO: modify using API
//        client->fetchnodes();
    }
}

// ephemeral session result
void DemoApp::ephemeral_result(error e)
{
    if (e)
    {
        cout << "Ephemeral session error (" << errorstring(e) << ")" << endl;
    }
}

// signup link send request result
void DemoApp::sendsignuplink_result(error e)
{
    if (e)
    {
        cout << "Unable to send signup link (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Thank you. Please check your e-mail and enter the command signup followed by the confirmation link." << endl;
    }
}

// signup link query result
void DemoApp::querysignuplink_result(handle uh, const char* email, const char* name, const byte* pwc, const byte* kc,
                                     const byte* c, size_t len)
{
    cout << "Ready to confirm user account " << email << " (" << name << ") - enter confirm to execute." << endl;

    signupemail = email;
    signupcode.assign((char*) c, len);
    memcpy(signuppwchallenge, pwc, sizeof signuppwchallenge);
    memcpy(signupencryptedmasterkey, pwc, sizeof signupencryptedmasterkey);
}

// signup link query failed
void DemoApp::querysignuplink_result(error e)
{
    cout << "Signuplink confirmation failed (" << errorstring(e) << ")" << endl;
}

// signup link (account e-mail) confirmation result
void DemoApp::confirmsignuplink_result(error e)
{
    if (e)
    {
        cout << "Signuplink confirmation failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "Signup confirmed, logging in..." << endl;
        client->login(signupemail.c_str(), pwkey);
    }
}

// asymmetric keypair configuration result
void DemoApp::setkeypair_result(error e)
{
    if (e)
    {
        cout << "RSA keypair setup failed (" << errorstring(e) << ")" << endl;
    }
    else
    {
        cout << "RSA keypair added. Account setup complete." << endl;
    }
}

void DemoApp::ephemeral_result(handle uh, const byte* pw)
{
    char buf[SymmCipher::KEYLENGTH * 4 / 3 + 3];

    cout << "Ephemeral session established, session ID: ";
    Base64::btoa((byte*) &uh, sizeof uh, buf);
    cout << buf << "#";
    Base64::btoa(pw, SymmCipher::KEYLENGTH, buf);
    cout << buf << endl;

    client->fetchnodes();
}

// password change result
void DemoApp::changepw_result(error e)
{
    if (e)
    {
        cout << "Password update failed: " << errorstring(e) << endl;
    }
    else
    {
        cout << "Password updated." << endl;
    }
}

// node export failed
void DemoApp::exportnode_result(error e)
{
    if (e)
    {
        cout << "Export failed: " << errorstring(e) << endl;
    }
}

void DemoApp::exportnode_result(handle h, handle ph)
{
    Node* n;

    if ((n = client->nodebyhandle(h)))
    {
        string path;
        char node[9];
        char key[FILENODEKEYLENGTH * 4 / 3 + 3];

        nodepath(h, &path);

        cout << "Exported " << path << ": ";

        Base64::btoa((byte*) &ph, MegaClient::NODEHANDLE, node);

        // the key
        if (n->type == FILENODE)
        {
            Base64::btoa((const byte*) n->nodekey.data(), FILENODEKEYLENGTH, key);
        }
        else if (n->sharekey)
        {
            Base64::btoa(n->sharekey->key, FOLDERNODEKEYLENGTH, key);
        }
        else
        {
            cout << "No key available for exported folder" << endl;
            return;
        }

        cout << "https://mega.co.nz/#" << (n->type ? "F" : "") << "!" << node << "!" << key << endl;
    }
    else
    {
        cout << "Exported node no longer available" << endl;
    }
}

// the requested link could not be opened
void DemoApp::openfilelink_result(error e)
{
    if (e)
    {
        cout << "Failed to open link: " << errorstring(e) << endl;
    }
}

// the requested link was opened successfully - import to cwd
void DemoApp::openfilelink_result(handle ph, const byte* key, m_off_t size,
                                  string* a, string* fa, int)
{
    Node* n;

    if (!key)
    {
        cout << "File is valid, but no key was provided." << endl;
        return;
    }

    // check if the file is decryptable
    string attrstring;
    string keystring;

    attrstring.resize(a->length()*4/3+4);
    attrstring.resize(Base64::btoa((const byte *)a->data(),a->length(), (char *)attrstring.data()));

    SymmCipher nodeKey;
    keystring.assign((char*)key,FILENODEKEYLENGTH);
    nodeKey.setkey(key, FILENODE);

    byte *buf = Node::decryptattr(&nodeKey,attrstring.c_str(),attrstring.size());
    if(!buf)
    {
        cout << "The file won't be imported, the provided key is invalid." << endl;
    }
    else if (client->loggedin() != NOTLOGGEDIN && (n = client->nodebyhandle(cwd)))
    {
        NewNode* newnode = new NewNode[1];

        // set up new node as folder node
        newnode->source = NEW_PUBLIC;
        newnode->type = FILENODE;
        newnode->nodehandle = ph;
        newnode->parenthandle = UNDEF;

        newnode->nodekey.assign((char*)key, FILENODEKEYLENGTH);

        newnode->attrstring = new string(*a);

        client->putnodes(n->nodehandle, newnode, 1);
    }
    else
    {
        cout << "Need to be logged in to import file links." << endl;
    }

    delete [] buf;
}

void DemoApp::checkfile_result(handle h, error e)
{
    cout << "Link check failed: " << errorstring(e) << endl;
}

void DemoApp::checkfile_result(handle h, error e, byte* filekey, m_off_t size, m_time_t ts, m_time_t tm, string* filename,
                               string* fingerprint, string* fileattrstring)
{
    cout << "Name: " << *filename << ", size: " << size;

    if (fingerprint->size())
    {
        cout << ", fingerprint available";
    }

    if (fileattrstring->size())
    {
        cout << ", has attributes";
    }

    cout << endl;

    if (e)
    {
        cout << "Not available: " << errorstring(e) << endl;
    }
    else
    {
        cout << "Initiating download..." << endl;
        //TODO: modify using API
//        AppFileGet* f = new AppFileGet(NULL, h, filekey, size, tm, filename, fingerprint);
//        f->appxfer_it = appxferq[GET].insert(appxferq[GET].end(), f);
//        client->startxfer(GET, f);
    }
}

bool DemoApp::pread_data(byte* data, m_off_t len, m_off_t pos, void* appdata)
{
    cout << "Received " << len << " partial read byte(s) at position " << pos << ": ";
    fwrite(data, 1, len, stdout);
    cout << endl;

    return true;
}

dstime DemoApp::pread_failure(error e, int retry, void* appdata)
{
    if (retry < 5)
    {
        cout << "Retrying read (" << errorstring(e) << ", attempt #" << retry << ")" << endl;
        return (dstime)(retry*10);
    }
    else
    {
        cout << "Too many failures (" << errorstring(e) << "), giving up" << endl;
        return ~(dstime)0;
    }
}

// reload needed
void DemoApp::reload(const char* reason)
{
    cout << "Reload suggested (" << reason << ") - use 'reload' to trigger" << endl;
}

// reload initiated
void DemoApp::clearing()
{
    LOG_debug << "Clearing all nodes/users..."; CLEAN_debug;
}

// nodes have been modified
// (nodes with their removed flag set will be deleted immediately after returning from this call,
// at which point their pointers will become invalid at that point.)
void DemoApp::nodes_updated(Node** n, int count)
{
    int c[2][6] = { { 0 } };

    if (n)
    {
        while (count--)
        {
            if ((*n)->type < 6)
            {
                c[!(*n)->changed.removed][(*n)->type]++;
                n++;
            }
        }
    }
    else
    {
        for (node_map::iterator it = client->nodes.begin(); it != client->nodes.end(); it++)
        {
            if (it->second->type < 6)
            {
                c[1][it->second->type]++;
            }
        }
    }

    nodestats(c[1], "added or updated");
    nodestats(c[0], "removed");

    if (ISUNDEF(cwd))
    {
        cwd = client->rootnodes[0];
    }
}

// nodes now (almost) current, i.e. no server-client notifications pending
void DemoApp::nodes_current()
{
    LOG_debug << "Nodes current."; CLEAN_debug;
}

void DemoApp::enumeratequotaitems_result(handle, unsigned, unsigned, unsigned, unsigned, unsigned, const char*)
{
    // FIXME: implement
}

void DemoApp::enumeratequotaitems_result(error)
{
    // FIXME: implement
}

void DemoApp::additem_result(error)
{
    // FIXME: implement
}

void DemoApp::checkout_result(error)
{
    // FIXME: implement
}

void DemoApp::checkout_result(const char*)
{
    // FIXME: implement
}

// display account details/history
void DemoApp::account_details(AccountDetails* ad, bool storage, bool transfer, bool pro, bool purchases,
                              bool transactions, bool sessions)
{
    char timebuf[32], timebuf2[32];

    if (storage)
    {
        cout << "\tAvailable storage: " << ad->storage_max << " byte(s)" << endl;

        for (unsigned i = 0; i < sizeof rootnodenames/sizeof *rootnodenames; i++)
        {
            NodeStorage* ns = &ad->storage[client->rootnodes[i]];

            cout << "\t\tIn " << rootnodenames[i] << ": " << ns->bytes << " byte(s) in " << ns->files << " file(s) and " << ns->folders << " folder(s)" << endl;
        }
    }

    if (transfer)
    {
        if (ad->transfer_max)
        {
            cout << "\tTransfer in progress: " << ad->transfer_own_reserved << "/" << ad->transfer_srv_reserved << endl;
            cout << "\tTransfer completed: " << ad->transfer_own_used << "/" << ad->transfer_srv_used << " of "
                 << ad->transfer_max << " ("
                 << (100 * (ad->transfer_own_used + ad->transfer_srv_used) / ad->transfer_max) << "%)" << endl;
            cout << "\tServing bandwidth ratio: " << ad->srv_ratio << "%" << endl;
        }

        if (ad->transfer_hist_starttime)
        {
            time_t t = time(NULL) - ad->transfer_hist_starttime;

            cout << "\tTransfer history:\n";

            for (unsigned i = 0; i < ad->transfer_hist.size(); i++)
            {
                t -= ad->transfer_hist_interval;
                cout << "\t\t" << t;
                if (t < ad->transfer_hist_interval)
                {
                    cout << " second(s) ago until now: ";
                }
                else
                {
                    cout << "-" << t - ad->transfer_hist_interval << " second(s) ago: ";
                }
                cout << ad->transfer_hist[i] << " byte(s)" << endl;
            }
        }

        if (ad->transfer_limit)
        {
            cout << "Per-IP transfer limit: " << ad->transfer_limit << endl;
        }
    }

    if (pro)
    {
        cout << "\tPro level: " << ad->pro_level << endl;
        cout << "\tSubscription type: " << ad->subscription_type << endl;
        cout << "\tAccount balance:" << endl;

        for (vector<AccountBalance>::iterator it = ad->balances.begin(); it != ad->balances.end(); it++)
        {
            printf("\tBalance: %.3s %.02f\n", it->currency, it->amount);
        }
    }

    if (purchases)
    {
        cout << "Purchase history:" << endl;

        for (vector<AccountPurchase>::iterator it = ad->purchases.begin(); it != ad->purchases.end(); it++)
        {
            time_t ts = it->timestamp;
            strftime(timebuf, sizeof timebuf, "%c", localtime(&ts));
            printf("\tID: %.11s Time: %s Amount: %.3s %.02f Payment method: %d\n", it->handle, timebuf, it->currency,
                   it->amount, it->method);
        }
    }

    if (transactions)
    {
        cout << "Transaction history:" << endl;

        for (vector<AccountTransaction>::iterator it = ad->transactions.begin(); it != ad->transactions.end(); it++)
        {
            time_t ts = it->timestamp;
            strftime(timebuf, sizeof timebuf, "%c", localtime(&ts));
            printf("\tID: %.11s Time: %s Delta: %.3s %.02f\n", it->handle, timebuf, it->currency, it->delta);
        }
    }

    if (sessions)
    {
        cout << "Currently Active Sessions:" << endl;
        for (vector<AccountSession>::iterator it = ad->sessions.begin(); it != ad->sessions.end(); it++)
        {
            if (it->alive)
            {
                time_t ts = it->timestamp;
                strftime(timebuf, sizeof timebuf, "%c", localtime(&ts));
                ts = it->mru;
                strftime(timebuf2, sizeof timebuf, "%c", localtime(&ts));

                char id[12];
                Base64::btoa((byte*)&(it->id), sizeof(it->id), id);

                if (it->current)
                {
                    printf("\t* Current Session\n");
                }
                printf("\tSession ID: %s\n\tSession start: %s\n\tMost recent activity: %s\n\tIP: %s\n\tCountry: %.2s\n\tUser-Agent: %s\n\t-----\n",
                        id, timebuf, timebuf2, it->ip.c_str(), it->country, it->useragent.c_str());
            }
        }

        if(client->debugstate())
        {
            cout << endl << "Full Session history:" << endl;

            for (vector<AccountSession>::iterator it = ad->sessions.begin(); it != ad->sessions.end(); it++)
            {
                time_t ts = it->timestamp;
                strftime(timebuf, sizeof timebuf, "%c", localtime(&ts));
                ts = it->mru;
                strftime(timebuf2, sizeof timebuf, "%c", localtime(&ts));
                printf("\tSession start: %s\n\tMost recent activity: %s\n\tIP: %s\n\tCountry: %.2s\n\tUser-Agent: %s\n\t-----\n",
                        timebuf, timebuf2, it->ip.c_str(), it->country, it->useragent.c_str());
            }
        }
    }
}

// account details could not be retrieved
void DemoApp::account_details(AccountDetails* ad, error e)
{
    if (e)
    {
        cout << "Account details retrieval failed (" << errorstring(e) << ")" << endl;
    }
}

// account details could not be retrieved
void DemoApp::sessions_killed(handle sessionid, error e)
{
    if (e)
    {
        cout << "Session killing failed (" << errorstring(e) << ")" << endl;
        return;
    }

    if (sessionid == UNDEF)
    {
        cout << "All sessions except current have been killed" << endl;
    }
    else
    {
        char id[12];
        int size = Base64::btoa((byte*)&(sessionid), sizeof(sessionid), id);
        cout << "Session with id " << id << " has been killed" << endl;
    }
}


// user attribute update notification
void DemoApp::userattr_update(User* u, int priv, const char* n)
{
    cout << "Notification: User " << u->email << " -" << (priv ? " private" : "") << " attribute "
          << n << " added or updated" << endl;
}
*/
// main loop
void megacmd()
{
    char *saved_line = NULL;
    int saved_point = 0;

    rl_save_prompt();

    for (;;)
    {
        if (prompt == COMMAND)
        {
            // display put/get transfer speed in the prompt
            //TODO: modify using API
//            if (client->tslots.size() || responseprogress >= 0)
//            {
//                unsigned xferrate[2] = { 0 };
//                Waiter::bumpds();

//                for (transferslot_list::iterator it = client->tslots.begin(); it != client->tslots.end(); it++)
//                {
//                    if ((*it)->fa)
//                    {
//                        xferrate[(*it)->transfer->type]
//                            += (*it)->progressreported * 10 / (1024 * (Waiter::ds - (*it)->starttime + 1));
//                    }
//                }

//                strcpy(dynamicprompt, "MEGA");

//                if (xferrate[GET] || xferrate[PUT] || responseprogress >= 0)
//                {
//                    strcpy(dynamicprompt + 4, " (");

//                    if (xferrate[GET])
//                    {
//                        sprintf(dynamicprompt + 6, "In: %u KB/s", xferrate[GET]);

//                        if (xferrate[PUT])
//                        {
//                            strcat(dynamicprompt + 9, "/");
//                        }
//                    }

//                    if (xferrate[PUT])
//                    {
//                        sprintf(strchr(dynamicprompt, 0), "Out: %u KB/s", xferrate[PUT]);
//                    }

//                    if (responseprogress >= 0)
//                    {
//                        sprintf(strchr(dynamicprompt, 0), "%d%%", responseprogress);
//                    }

//                    strcat(dynamicprompt + 6, ")");
//                }

//                strcat(dynamicprompt + 4, "> ");
//            }
//            else
//            {
//                *dynamicprompt = 0;
//            }

            rl_callback_handler_install(*dynamicprompt ? dynamicprompt : prompts[COMMAND], store_line);

            // display prompt
            if (saved_line)
            {
                rl_replace_line(saved_line, 0);
                free(saved_line);
            }

            rl_point = saved_point;
            rl_redisplay();
        }

        // command editing loop - exits when a line is submitted or the engine requires the CPU
        for (;;)
        {
            //TODO: modify using API
//            int w = client->wait();

//            if (w & Waiter::HAVESTDIN)
            if (Waiter::HAVESTDIN)
            {
                if (prompt == COMMAND)
                {
                    rl_callback_read_char();
                }
                else
                {
                    console->readpwchar(pw_buf, sizeof pw_buf, &pw_buf_pos, &line);
                }
            }

//            if (w & Waiter::NEEDEXEC || line)
            if (Waiter::NEEDEXEC || line)
            {
                break;
            }
        }

        // save line
        saved_point = rl_point;
        saved_line = rl_copy_text(0, rl_end);

        // remove prompt
        rl_save_prompt();
        rl_replace_line("", 0);
        rl_redisplay();

        if (line)
        {
            // execute user command
            process_line(line);
            free(line);
            line = NULL;
        }

        // pass the CPU to the engine (nonblocking)
        //TODO: modify using API
//        client->exec();
    }
}


class LoggerForApi: public MegaLogger{
private:
    int level;
public:
    LoggerForApi()
    {
        this->level=MegaApi::LOG_LEVEL_ERROR;
    }

    void log(const char *time, int loglevel, const char *source, const char *message)
    {
        if (loglevel<=level)
        {
            cout << "[" << loglevel << "]" << message;
        }
    }

    void setLevel(int loglevel){
        this->level=loglevel;
    }
};

int main()
{
    SimpleLogger::setAllOutputs(&std::cout);


    // instantiate app components: the callback processor (DemoApp),
    // the HTTP I/O engine (WinHttpIO) and the MegaClient itself
    //TODO: modify using API
//    client = new MegaClient(new DemoApp, new CONSOLE_WAIT_CLASS,
//                            new HTTPIO_CLASS, new FSACCESS_CLASS,
//#ifdef DBACCESS_CLASS
//                            new DBACCESS_CLASS,
//#else
//                            NULL,
//#endif
//#ifdef GFX_CLASS
//                            new GFX_CLASS,
//#else
//                            NULL,
//#endif
//                            "SDKSAMPLE",
//                            "megacli/" TOSTRING(MEGA_MAJOR_VERSION)
//                            "." TOSTRING(MEGA_MINOR_VERSION)
//                            "." TOSTRING(MEGA_MICRO_VERSION));



    api=new MegaApi("BdARkQSQ",(const char*)NULL, "MegaCMD User Agent"); // TODO: store user agent somewhere, and use path to cache!
    LoggerForApi* apiLogger = new LoggerForApi(); //TODO: never deleted
//    apiLogger->setLevel(MegaApi::LOG_LEVEL_ERROR);
//    api->setLoggerObject(apiLogger);
//    api->setLogLevel(MegaApi::LOG_LEVEL_MAX);

    //TODO: use apiLogger for megacmd and keep on using the SimpleLogger for megaapi

    megaCmdListener = new MegaCmdListener(api,NULL); //TODO: never deleted
    megaCmdGlobalListener =  new MegaCmdGlobalListener(); //TODO: never deleted

    api->addGlobalListener(megaCmdGlobalListener);

    SimpleLogger::setLogLevel(logInfo);
//      SimpleLogger::setLogLevel(logDebug);
//    SimpleLogger::setLogLevel(logError);
//    SimpleLogger::setLogLevel(logFatal);

    console = new CONSOLE_CLASS;

#ifdef __linux__
    // prevent CTRL+C exit
    signal(SIGINT, sigint_handler);
#endif

    megacmd();
}

