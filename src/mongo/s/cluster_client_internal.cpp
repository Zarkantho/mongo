/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/cluster_client_internal.h"

#include <boost/scoped_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/type_changelog.h"
#include "mongo/s/type_mongos.h"
#include "mongo/s/type_shard.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::string;
    using std::vector;
    using mongoutils::str::stream;

    Status checkClusterMongoVersions(const ConnectionString& configLoc,
                                     const string& minMongoVersion)
    {
        scoped_ptr<ScopedDbConnection> connPtr;

        //
        // Find mongos pings in config server
        //

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(MongosType::ConfigNS,
                                                                      Query())));

            while (cursor->more()) {

                BSONObj pingDoc = cursor->next();

                MongosType ping;
                string errMsg;
                // NOTE: We don't care if the ping is invalid, legacy stuff will be
                if (!ping.parseBSON(pingDoc, &errMsg)) {
                    warning() << "could not parse ping document: " << pingDoc << causedBy(errMsg)
                              << endl;
                    continue;
                }

                string mongoVersion = "2.0";
                // Hack to determine older mongos versions from ping format
                if (ping.isWaitingSet()) mongoVersion = "2.2";
                if (ping.isMongoVersionSet() && ping.getMongoVersion() != "") {
                    mongoVersion = ping.getMongoVersion();
                }

                Date_t lastPing = ping.getPing();

                long long quietIntervalMillis = 0;
                Date_t currentJsTime = jsTime();
                if (currentJsTime >= lastPing) {
                    quietIntervalMillis = static_cast<long long>(currentJsTime - lastPing);
                }
                long long quietIntervalMins = quietIntervalMillis / (60 * 1000);

                // We assume that anything that hasn't pinged in 5 minutes is probably down
                if (quietIntervalMins >= 5) {
                    log() << "stale mongos detected " << quietIntervalMins << " minutes ago,"
                          << " network location is " << pingDoc["_id"].String()
                          << ", not checking version" << endl;
            	}
                else {
                    if (versionCmp(mongoVersion, minMongoVersion) < 0) {
                        return Status(ErrorCodes::RemoteValidationError,
                                      stream() << "version " << mongoVersion
                                               << " detected on mongos at "
                                               << ping.getName()
                                               << ", but version >= " << minMongoVersion
                                               << " required; you must wait 5 minutes "
                                               << "after shutting down a pre-" << minMongoVersion
                                               << " mongos");
                    }
                }
            }
        }
        catch (const DBException& e) {
            return e.toStatus("could not read mongos pings collection");
        }

        //
        // Load shards from config server
        //

        vector<HostAndPort> servers;

        try {
            ScopedDbConnection& conn = *connPtr;
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(ShardType::ConfigNS,
                                                                      Query())));

            while (cursor->more()) {

                BSONObj shardDoc = cursor->next();

                ShardType shard;
                string errMsg;
                if (!shard.parseBSON(shardDoc, &errMsg) || !shard.isValid(&errMsg)) {
                    connPtr->done();
                    return Status(ErrorCodes::UnsupportedFormat,
                                  stream() << "invalid shard " << shardDoc
                                           << " read from the config server" << causedBy(errMsg));
                }

                ConnectionString shardLoc = ConnectionString::parse(shard.getHost(), errMsg);
                if (shardLoc.type() == ConnectionString::INVALID) {
                    connPtr->done();
                    return Status(ErrorCodes::UnsupportedFormat,
                                  stream() << "invalid shard host " << shard.getHost()
                                           << " read from the config server" << causedBy(errMsg));
                }

                vector<HostAndPort> shardServers = shardLoc.getServers();
                servers.insert(servers.end(), shardServers.begin(), shardServers.end());
            }
        }
        catch (const DBException& e) {
            return e.toStatus("could not read shards collection");
        }

        connPtr->done();

        // Add config servers to list of servers to check version against
        vector<HostAndPort> configServers = configLoc.getServers();
        servers.insert(servers.end(), configServers.begin(), configServers.end());

        //
        // We've now got all the shard info from the config server, start contacting the shards
        // and config servers and verifying their versions.
        //


        for (vector<HostAndPort>::iterator serverIt = servers.begin();
                serverIt != servers.end(); ++serverIt)
        {
            // Note: This will *always* be a single-host connection
            ConnectionString serverLoc(*serverIt);
            dassert(serverLoc.type() == ConnectionString::MASTER || 
                    serverLoc.type() == ConnectionString::CUSTOM); // for dbtests

            log() << "checking that version of host " << serverLoc << " is compatible with "
                  << minMongoVersion << endl;

            scoped_ptr<ScopedDbConnection> serverConnPtr;

            bool resultOk;
            BSONObj buildInfo;

            try {
                serverConnPtr.reset(new ScopedDbConnection(serverLoc, 30));
                ScopedDbConnection& serverConn = *serverConnPtr;

                resultOk = serverConn->runCommand("admin",
                                                  BSON("buildInfo" << 1),
                                                  buildInfo);
            }
            catch (const DBException& e) {
                warning() << "could not run buildInfo command on " << serverLoc.toString() << " "
                          << causedBy(e) << ". Please ensure that this server is up and at a "
                                  "version >= "
                          << minMongoVersion;
                continue;
            }

            // TODO: Make running commands saner such that we can consolidate error handling
            if (!resultOk) {
                return Status(ErrorCodes::UnknownError,
                              stream() << DBClientConnection::getLastErrorString(buildInfo)
                                       << causedBy(buildInfo.toString()));
            }

            serverConnPtr->done();

            verify(buildInfo["version"].type() == String);
            string mongoVersion = buildInfo["version"].String();

            if (versionCmp(mongoVersion, minMongoVersion) < 0) {
                return Status(ErrorCodes::RemoteValidationError,
                              stream() << "version " << mongoVersion << " detected on mongo "
                              "server at " << serverLoc.toString() <<
                              ", but version >= " << minMongoVersion << " required");
            }
        }

        return Status::OK();
    }

    Status logConfigChange(const ConnectionString& configLoc,
                           const string& clientHost,
                           const string& ns,
                           const string& description,
                           const BSONObj& details)
    {
        //
        // The code for writing to the changelog collection exists elsewhere - we duplicate here to
        // avoid dependency issues.
        // TODO: Merge again once config.cpp is cleaned up.
        //

        string changeID = stream() << getHostNameCached() << "-" << terseCurrentTime() << "-"
                                   << OID::gen();

        ChangelogType changelog;
        changelog.setChangeID(changeID);
        changelog.setServer(getHostNameCached());
        changelog.setClientAddr(clientHost == "" ? "N/A" : clientHost);
        changelog.setTime(jsTime());
        changelog.setWhat(description);
        changelog.setNS(ns);
        changelog.setDetails(details);

        log() << "about to log new metadata event: " << changelog.toBSON() << endl;

        scoped_ptr<ScopedDbConnection> connPtr;

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;

            // TODO: better way here
            static bool createdCapped = false;
            if (!createdCapped) {

                try {
                    conn->createCollection(ChangelogType::ConfigNS, 1024 * 1024 * 10, true);
                }
                catch (const DBException& e) {
                    // don't care, someone else may have done this for us
                    // if there's still a problem, caught in outer try
                    LOG(1) << "couldn't create the changelog, continuing " << e << endl;
                }

                createdCapped = true;
            }
            connPtr->done();
        }
        catch (const DBException& e) {
            // if we got here, it means the config change is only in the log,
            // it didn't make it to config.changelog
            log() << "not logging config change: " << changeID << causedBy(e) << endl;
            return e.toStatus();
        }

        Status result = grid.catalogManager()->insert(ChangelogType::ConfigNS,
                                                      changelog.toBSON(),
                                                      NULL);
        if (!result.isOK()) {
            return Status(result.code(),
                          str::stream() << "failed to write to changelog: "
                                        << result.reason());
        }

        return result;
    }

    // Helper function for safe cursors
    DBClientCursor* _safeCursor(auto_ptr<DBClientCursor> cursor) {
        // TODO: Make error handling more consistent, it's annoying that cursors error out by
        // throwing exceptions *and* being empty
        uassert(16625, str::stream() << "cursor not found, transport error", cursor.get());
        return cursor.release();
    }

}
