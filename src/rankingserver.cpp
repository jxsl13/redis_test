#include "rankingserver.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>

CRankingServer::CRankingServer(std::string host, size_t port, uint32_t timeout, uint32_t reconnect_ms) : m_Host{host}, m_Port{port}
{
    // all possible fields are invalid nicks
    CPlayerStats tmp;
    m_InvalidNicknames = tmp.keys();

    m_ReconnectIntervalMilliseconds = reconnect_ms;
    try
    {
        m_Client.connect(m_Host, m_Port, nullptr, timeout, 0, reconnect_ms);
        if (m_Client.is_connected())
        {
            // no reconnection handling necessary
            std::lock_guard<std::mutex> lock(m_ReconnectHandlerMutex);
            m_IsReconnectHandlerRunning = false;
            std::cout << "[redis]: "
                      << "successfully connected to " << m_Host << ":" << m_Port << std::endl;
        }
    }
    catch (const cpp_redis::redis_error& e)
    {
        std::cout << "[redis]: "
                  << "failed to connect to " << m_Host << ":" << m_Port << std::endl;
        StartReconnectHandler();
    }
}

void CRankingServer::HandleReconnecting()
{
    while (!m_Client.is_connected())
    {
        try
        {
            m_Client.connect(m_Host, m_Port);
        }
        catch (const cpp_redis::redis_error& e)
        {
            std::cout << "[redis]: Reconnect failed...\n";
        }

        // wait
        std::this_thread::sleep_for(std::chrono::milliseconds(m_ReconnectIntervalMilliseconds));

        m_ReconnectHandlerMutex.lock();
        if (!m_IsReconnectHandlerRunning)
        {
            std::cout << "[redis]: Shutting down reconnect handler.\n";
            // forceful shutdown
            m_ReconnectHandlerMutex.unlock();

            // forceful shutdown, is done, when the ranking server is
            // shutting down.
            CleanupBacklog();
            return;
        }
        m_ReconnectHandlerMutex.unlock();
    }

    // connection established
    std::lock_guard<std::mutex> lock(m_ReconnectHandlerMutex);
    m_IsReconnectHandlerRunning = false;

    std::cout << "[redis]: Successfully reconnected!\n";
    // if connection established, try purging the db backlog
    CleanupBacklog();
}

void CRankingServer::StartReconnectHandler()
{
    std::lock_guard<std::mutex> lock(m_ReconnectHandlerMutex);
    if (m_IsReconnectHandlerRunning)
        return; // already running

    m_IsReconnectHandlerRunning = true;
    m_Futures.push_back(std::async(std::launch::async, &CRankingServer::HandleReconnecting, this));
}

bool CRankingServer::IsValidNickname(const std::string& nickname, const std::string& prefix)
{
    if (nickname.size() == 0)
        return false; // empty string nick -> no rankings for you
    else if (m_InvalidNicknames.size() == 0)
        return true; // no invalid nicks -> your nick is valid

    for (auto name : m_InvalidNicknames)
    {
        if (nickname == name)
            return false;
        else if (nickname == (prefix + name))
            return false;
    }
    return true;
}

CPlayerStats CRankingServer::GetRankingSync(std::string nickname, std::string prefix)
{
    CPlayerStats stats;

    try
    {
        std::future<cpp_redis::reply> existsFuture = m_Client.exists({nickname});
        m_Client.sync_commit();

        cpp_redis::reply existsReply = existsFuture.get();

        if (existsReply.as_integer())
        {
            std::future<cpp_redis::reply> getFuture = m_Client.hmget(nickname, stats.keys(prefix));

            m_Client.sync_commit();
            cpp_redis::reply reply = getFuture.get();

            std::vector<cpp_redis::reply> result = reply.as_array();

            // set every key.
            int idx = 0;
            for (auto& key : stats.keys())
            {
                if (result.at(idx).is_null())
                {
                    // result is null
                    // key not found
                    stats.Invalidate();
                    break; // entry does not exist yet.
                }
                else if (result.at(idx).is_string())
                {
                    stats[key] = std::stoi(result.at(idx).as_string());
                }
                else if (result.at(idx).is_integer())
                {
                    stats[key] = result.at(idx).as_integer();
                }
                else
                {
                    std::cout << "[redis_error]: unkown result type" << std::endl;
                    stats.Invalidate();
                    break;
                }
                idx++;
            }
        }
        return stats;
    }
    catch (const cpp_redis::redis_error& e)
    {
        std::cout << "[redis]: lost connection: " << e.what() << std::endl;
        stats.Invalidate();
        StartReconnectHandler();
        return stats;
    }
}

void CRankingServer::GetRanking(std::string nickname, std::function<void(CPlayerStats&)> callback, std::string prefix)
{
    // fix disconnect state that might have occurred in the mean time
    CleanupFutures();

    if (callback)
    {
        m_Futures.push_back(std::async(
            std::launch::async, [this](std::string nick, std::function<void(CPlayerStats&)> cb, std::string pref) {
                CPlayerStats stats = this->GetRankingSync(nick, pref); // get data from server
                cb(stats);                                             // call callback on data
            },
            nickname, callback, prefix));
    }
}

void CRankingServer::GetTopRanking(int topNumber, std::string key, std::function<void(std::vector<std::pair<std::string, CPlayerStats> >&)> callback, std::string prefix, bool biggestFirst)
{
    m_Futures.push_back(std::async(
        std::launch::async, [this](int topNum, std::string field, decltype(callback) cb, std::string pref, bool bigFirst) {
            std::vector<std::pair<std::string, CPlayerStats> > result = this->GetTopRankingSync(topNum, field, pref, bigFirst);
            cb(result);
        },
        topNumber, key, callback, prefix, biggestFirst));
}

std::vector<std::pair<std::string, CPlayerStats> > CRankingServer::GetTopRankingSync(int topNumber, std::string key, std::string prefix, bool biggestFirst)
{
    std::string index = prefix + key;

    try
    {
        std::future<cpp_redis::reply> existsFuture = m_Client.exists({index});
        m_Client.sync_commit();

        cpp_redis::reply existsReply = existsFuture.get();

        if (existsReply.is_integer() && existsReply.as_integer() != 0)
        {
            // specified index exists
            std::future<cpp_redis::reply> resultFuture;

            if (biggestFirst)
            {
                resultFuture = m_Client.zrevrangebyscore(index, "+inf", "0", 0, topNumber);
            }
            else
            {
                resultFuture = m_Client.zrangebyscore(index, "0", "+inf", 0, topNumber);
            }

            m_Client.sync_commit();

            cpp_redis::reply result = resultFuture.get();

            if (result.is_array())
            {
                std::vector<std::pair<std::string, CPlayerStats> > sortedResult;

                for (auto& r : result.as_array())
                {
                    if (r.is_string())
                    {
                        sortedResult.push_back({r.as_string(), {/* empty*/}});
                    }
                    else
                    {
                        throw cpp_redis::redis_error("Expected string as nickname.");
                    }
                }

                for (auto& [nickname, stats] : sortedResult)
                {
                    stats = GetRankingSync(nickname, prefix);
                }

                return sortedResult;
            }
            else
            {
                throw cpp_redis::redis_error("Expected array return value of z[rev]rangebyscore(...)");
            }
        }
        else
        {
            throw cpp_redis::redis_error("exists: expected integer reply");
        }
    }
    catch (const cpp_redis::redis_error& e)
    {
        std::cerr << e.what() << '\n';
        return {};
    }
}

void CRankingServer::DeleteRanking(std::string nickname, std::string prefix)
{
    CleanupFutures();

    m_Futures.push_back(std::async(std::launch::async, &CRankingServer::DeleteRankingSync, this, nickname, prefix));
}

void CRankingServer::UpdateRankingSync(std::string nickname, CPlayerStats stats, std::string prefix)
{
    try
    {
        CPlayerStats dbStats;

        dbStats = GetRankingSync(nickname, prefix);

        if (!dbStats.IsValid())
            throw cpp_redis::redis_error(""); // jump to lost connection or nick not found

        dbStats += stats;

        std::future<cpp_redis::reply> setFuture = m_Client.hmset(nickname, dbStats.GetStringPairs(prefix));

        // create/update index for every key
        std::vector<std::string> options = {};
        std::vector<std::future<cpp_redis::reply> > indexFutures;
        for (auto& key : dbStats.keys())
        {
            indexFutures.push_back(
                m_Client.zadd(prefix + key,
                              options,
                              {{std::to_string(dbStats[key]), nickname}}));
        }

        m_Client.sync_commit();
        for (auto& f : indexFutures)
        {
            cpp_redis::reply r = f.get();
            // retrieve results
            // don't do anything with it.
        }
    }
    catch (const cpp_redis::redis_error& e)
    {
        if (!m_Client.is_connected())
        {
            std::cout << "[redis]: lost connection: " << e.what() << std::endl;
            StartReconnectHandler();
            std::lock_guard<std::mutex> lock(m_BacklogMutex);
            m_Backlog.push_back({"update", nickname, stats, prefix});
        }
        else
        {
            std::cout << "invalid nickname: " << nickname << std::endl;
            // invalid player nick retrieved.
            // don't do anything then.
        }

        return;
    }
}

bool CRankingServer::UpdateRanking(std::string nickname, CPlayerStats stats, std::string prefix)
{
    CleanupFutures();

    if (!IsValidNickname(nickname, prefix))
        return false;

    m_Futures.push_back(std::async(std::launch::async, &CRankingServer::UpdateRankingSync, this, nickname, stats, prefix));

    return true;
}

void CRankingServer::DeleteRankingSync(std::string nickname, std::string prefix)
{
    try
    {
        CPlayerStats stats;

        std::future<cpp_redis::reply> existsFuture = m_Client.exists({nickname});
        m_Client.sync_commit();

        cpp_redis::reply existsReply = existsFuture.get();

        if (existsReply.as_integer()) // exists
        {
            // check if type is hash
            std::future<cpp_redis::reply> typeFuture = m_Client.type(nickname);
            m_Client.sync_commit();
            cpp_redis::reply typeReply = typeFuture.get();

            if (typeReply.is_string() && typeReply.as_string() != "hash")
            {
                std::cout << "Deleting: " << nickname << " failed, type is not hash: " << typeReply.as_string() << std::endl;
                return; // invalid object
            }
            else if (!typeReply.is_string())
            {
                std::cout << "Deleting: " << nickname << " failed, reply not a string." << std::endl;
                return;
            }

            // all keys represent individual index names
            std::future<cpp_redis::reply> allKeysFuture = m_Client.hkeys(nickname);
            m_Client.sync_commit();
            cpp_redis::reply keysReply = allKeysFuture.get();

            // contains keys that ought to be deleted
            // these keys also identify the sorted set indices
            std::vector<std::string> keys;
            if (keysReply.is_array())
            {
                for (auto& key : keysReply.as_array())
                {
                    if (key.is_string())
                    {
                        keys.push_back(key.as_string());
                    }
                    else if (key.is_integer())
                    {
                        keys.push_back(std::to_string(key.as_integer()));
                    }
                    else
                    {
                        // invalid case
                        continue;
                    }
                }
            }
            else
            {
                std::cout << "failed to retrieve all field names, returned not an array." << std::endl;
                return;
            }

            std::future<cpp_redis::reply> delFuture;
            std::vector<std::future<cpp_redis::reply> > delIndicesFutures;

            if (prefix.size() > 0)
            {
                // remove keys that don't have the correct prefix
                keys.erase(std::remove_if(keys.begin(), keys.end(), [&prefix](const std::string& key) {
                               if (key.size() < prefix.size())
                               {
                                   return true; // delete invalid key from vector
                               }

                               // expect key and prefix length to be at least equal
                               for (size_t i = 0; i < prefix.size(); i++)
                               {
                                   // prefix doesn't match
                                   if (key.at(i) != prefix.at(i))
                                       return true; // remove key, cuz invalid prefix
                               }
                               // full prefix matches, don't remove
                               return false;
                           }),
                           keys.end());

                if (keys.size() == 0)
                {
                    std::cout << "no keys matching prefix: '" << prefix << "'. Did not delete any entries for " << nickname << std::endl;
                    return;
                }

                // delete only specified fields with prefix
                delFuture = m_Client.hdel(nickname, keys);
            }
            else
            {
                // delete all player data
                delFuture = m_Client.del({nickname});
            }

            // remove idices

            for (auto& key : keys)
            {
                delIndicesFutures.push_back(m_Client.zrem(key, {nickname}));
            }

            m_Client.sync_commit();
            cpp_redis::reply reply = delFuture.get();

            int result = 0;
            if (reply.is_integer())
            {
                result = reply.as_integer();
            }
            else
            {
                std::cout << "Invalid result, expected integer: " << reply << std::endl;
            }

            if (!result)
            {
                throw cpp_redis::redis_error("deletion failed");
            }

            int tmp = 0;
            for (auto& f : delIndicesFutures)
            {
                cpp_redis::reply delIndexReply = f.get();

                if (delIndexReply.is_integer())
                {
                    tmp = delIndexReply.is_integer();
                    if (!tmp)
                    {
                        std::cout << "Failed to delete an index: " << delIndexReply << std::endl;
                    }
                }
                else
                {
                    throw cpp_redis::redis_error("failed to delete index: ");
                }
            }
        }
    }
    catch (const cpp_redis::redis_error& e)
    {
        // connection lost -> push tast into backlog
        if (!m_Client.is_connected())
        {
            std::cout << "[redis]: lost connection: " << e.what() << std::endl;
            std::lock_guard<std::mutex> lock(m_BacklogMutex);
            m_Backlog.push_back({"delete", nickname, CPlayerStats(), prefix});
            StartReconnectHandler();
        }
    }
}

void CRankingServer::CleanupBacklog()
{
    std::lock_guard<std::mutex> lock(m_BacklogMutex);
    if (m_Backlog.size() > 0)
    {
        // we start new threads from here.
        // if those threads fail, they will keep on waiting
        // for this mutex, until they add their failed information
        // back to the backlog.

        int counter = 0;
        for (size_t i = 0; i < m_Backlog.size(); i++)
        {
            auto [action, nickname, stats, prefix] = m_Backlog.back();
            m_Backlog.pop_back();

            if (action == "update")
            {
                UpdateRanking(nickname, stats, prefix);
                counter++;
            }
            else if (action == "delete")
            {
                DeleteRanking(nickname, prefix);
                counter++;
            }
        }

        std::cout << "[redis]: Cleaned up " << counter << " backlog tasks." << std::endl;
    }
    else
    {
        // backlog empty
    }
}

void CRankingServer::CleanupFutures()
{
    if (m_Futures.size() == 0)
        return;

    auto it = std::remove_if(m_Futures.begin(), m_Futures.end(), [&](std::future<void>& f) {
        if (!f.valid())
            return true;

        if (std::future_status::ready == f.wait_for(std::chrono::milliseconds(0)))
        {
            try
            {
                f.get();
            }
            catch (const std::exception& e)
            {
                std::cerr << e.what() << '\n';
            }

            return true;
        }
        return false;
    });
    m_Futures.erase(it, m_Futures.end());
}

void CRankingServer::AwaitFutures()
{
    for (auto& f : m_Futures)
    {
        if (f.valid())
        {
            f.wait();
            try
            {
                f.get();
            }
            catch (const cpp_redis::redis_error& e)
            {
                std::cerr << e.what() << '\n';
            }
        }
    }
}

CRankingServer::~CRankingServer()
{
    // we still fail to reconnect at shutdown -> force shutdown
    m_ReconnectHandlerMutex.lock();
    m_IsReconnectHandlerRunning = false;
    m_ReconnectHandlerMutex.unlock();

    AwaitFutures();

    if (m_Client.is_connected())
    {
        m_Client.disconnect(true);
        std::cout << "[redis]: disconnected from database" << std::endl;
    }
}
