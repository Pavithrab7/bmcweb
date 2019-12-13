#pragma once

#include <app.h>

#include <boost/algorithm/string.hpp>
#include <boost/container/flat_map.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace crow
{
namespace ibm_mc_lock
{

namespace fs = std::filesystem;
using stype = std::string;

/*----------------------------------------
|Segment flags : LockFlag | SegmentLength|
------------------------------------------*/

using segmentflags = std::vector<std::pair<stype, uint32_t>>;

// Lockrequest = session-id | hmc-id | locktype | resourceid | segmentinfo
using lockrequest = std::tuple<stype, stype, stype, uint64_t, segmentflags>;

using lockrequests = std::vector<lockrequest>;
using rc =
    std::pair<bool, std::variant<uint32_t, std::pair<uint32_t, lockrequest>>>;
using rcrelaselock = std::pair<bool, std::pair<uint32_t, lockrequest>>;
using rcgetlocklist = std::pair<
    bool,
    std::variant<std::string, std::vector<std::pair<uint32_t, lockrequests>>>>;
using listoftransactionIDs = std::vector<uint32_t>;
using rcacquirelock = std::pair<bool, std::variant<rc, std::pair<bool, int>>>;
using rcreleaselockapi = std::pair<bool, std::variant<bool, rcrelaselock>>;

class lock
{

  private:
    uint32_t transactionID;
    std::map<uint32_t, lockrequests> locktable;

    /*
     * This function implements the logic for validating an incomming
     * lock request/requests.
     *
     * Returns : True (if Valid)
     * Returns : False (if not a Valid lock request)
     */

    bool isvalidlockrequest(const lockrequest);

    /*
     * This function implements the logic of checking if the incomming
     * multi-lock request is not having conflicting requirements.
     *
     * Returns : True (if conflicting)
     * Returns : False (if not conflicting)
     */

    bool isconflictrequest(const lockrequests);
    /*
     * Implements the core algorithm to find the conflicting
     * lock requests.
     *
     * This functions takes two lock requests and check if both
     * are conflicting to each other.
     *
     * Returns : True (if conflicting)
     * Returns : False (if not conflicting)
     */
    bool isconflictrecord(const lockrequest, const lockrequest);

    /*
     * This function implements the logic of checking the conflicting
     * locks from a incomming single/multi lock requests with the already
     * existing lock request in the lock table.
     *
     */

    rc isconflictwithtable(const lockrequests);
    /*
     * This function implements the logic of checking the ownership of the
     * lock from the releaselock request.
     *
     * Returns : True (if the requesting HMC & Session owns the lock(s))
     * Returns : False (if the request HMC or Session does not own the lock(s))
     */

    rcrelaselock isitmylock(const listoftransactionIDs,
                            std::pair<stype, stype>);

    /*
     * This function validates the the list of transactionID's and returns false
     * if the transaction ID is not valid & not present in the lock table
     */

    bool validaterids(const listoftransactionIDs);

    /*
     * This function releases the locks that are already obtained by the
     * requesting Management console.
     */

    void releaselock(const listoftransactionIDs);

    /*
     * This function implements the algorithm for checking the respective
     * bytes of the resource id based on the lock management algorithm.
     */
    bool checkbyte(uint64_t, uint64_t, uint32_t);

    /*
     * This functions implements a counter that generates a unique 32 bit
     * number for every successful transaction. This number will be used by
     * the Management Console for debug.
     */
    uint32_t generateTransactionID();

  public:
    /*
     * This function implements the logic for acquiring a lock on a
     * resource if the incomming request is legitimate without any
     * conflicting requirements & without any conflicting requirement
     * with the exsiting locks in the lock table.
     *
     */

    rcacquirelock Acquirelock(const lockrequests);

    /*
     * This function implements the logic for releasing the lock that are
     * owned by a management console session.
     *
     * The locks can be released by two ways
     *  - Using list of transaction ID's
     *  - Using a Session ID
     *
     * Client can choose either of the ways by using `Type` JSON key.
     *
     */
    rcreleaselockapi Releaselock(const listoftransactionIDs,
                                 std::pair<stype, stype>);

    lock()
    {
        transactionID = 0;
    }

} lockobject;

rcreleaselockapi lock::Releaselock(listoftransactionIDs p,
                                   std::pair<stype, stype> ids)
{

    bool status = validaterids(p);

    if (!status)
    {
        // Validation of rids failed
        BMCWEB_LOG_DEBUG << "Not a Valid request id";
        return std::make_pair(false, status);
    }
    else
    {
        // Validation passed, check if all the locks are owned by the
        // requesting HMC
        auto status = isitmylock(p, std::make_pair(ids.first, ids.second));
        if (status.first)
        {
            // The current hmc owns all the locks, so we can release
            // them
            releaselock(p);
        }
        return std::make_pair(true, status);
    }
    return std::make_pair(false, status);
}

rcacquirelock lock::Acquirelock(const lockrequests lockrequeststructure)
{

    // validate the lock request

    for (auto lockrecord : lockrequeststructure)
    {
        const lockrequest &request = lockrecord;
        bool status = isvalidlockrequest(request);
        if (!status)
        {
            BMCWEB_LOG_DEBUG << "Not a Valid record";
            BMCWEB_LOG_DEBUG << "Bad json in request";
            return std::make_pair(true, std::make_pair(status, 0));
        }
    }
    // check for conflict record

    const lockrequests &multirequest = lockrequeststructure;
    bool status = isconflictrequest(multirequest);

    if (status)
    {
        BMCWEB_LOG_DEBUG << "There is a conflict within itself";
        return std::make_pair(true, std::make_pair(status, 1));
    }
    else
    {
        BMCWEB_LOG_DEBUG << "The request is not conflicting within itself";

        // Need to check for conflict with the locktable entries.

        auto conflict = isconflictwithtable(multirequest);

        BMCWEB_LOG_DEBUG << "Done with checking conflict with the locktable";
        return std::make_pair(false, conflict);
    }

    return std::make_pair(true, std::make_pair(true, 1));
}

void lock::releaselock(const std::vector<uint32_t> refrids)
{
    for (auto id : refrids)
    {
        if (locktable.erase(id))
        {
            BMCWEB_LOG_DEBUG << "Removing the locks with transaction ID : "
                             << id;
        }

        else
        {
            BMCWEB_LOG_DEBUG << "Removing the locks from the lock table "
                                "failed, tranasction ID: "
                             << id;
        }
    }
}

rcrelaselock lock::isitmylock(const std::vector<uint32_t> refrids,
                              std::pair<stype, stype> ids)
{
    for (auto id : refrids)
    {
        // Just need to compare the client id of the first lock records in the
        // complete lock row(in the map), because the rest of the lock records
        // would have the same client id

        std::string expectedclientid = std::get<1>(locktable[id][0]);
        std::string expectedsessionid = std::get<0>(locktable[id][0]);

        if ((expectedclientid == ids.first) &&
            (expectedsessionid == ids.second))
        {
            // It is owned by the currently request hmc
            BMCWEB_LOG_DEBUG << "Lock is owned  by the current hmc";
        }
        else
        {
            BMCWEB_LOG_DEBUG << "Lock is not owned by the current hmc";
            return std::make_pair(false, std::make_pair(id, locktable[id][0]));
        }
    }
    return std::make_pair(true, std::make_pair(0, lockrequest()));
}

bool lock::validaterids(const std::vector<uint32_t> refrids)
{
    for (auto id : refrids)
    {
        auto search = locktable.find(id);

        if (search != locktable.end())
        {
            BMCWEB_LOG_DEBUG << "Valid Transaction id";
            //  continue for the next rid
        }
        else
        {
            BMCWEB_LOG_DEBUG << "Atleast 1 inValid Request id";
            return false;
        }
    }
    return true;
}

bool lock::isvalidlockrequest(const lockrequest reflockrecord)
{

    // validate the locktype

    if (!((boost::equals(std::get<2>(reflockrecord), "Read") ||
           (boost::equals(std::get<2>(reflockrecord), "Write")))))
    {
        BMCWEB_LOG_DEBUG << "Validation of LockType Failed";
        BMCWEB_LOG_DEBUG << "Locktype : " << std::get<2>(reflockrecord);
        return false;
    }

    BMCWEB_LOG_DEBUG << static_cast<int>(std::get<4>(reflockrecord).size());

    // validate the number of segments
    // Allowed No of segments are between 2 and 6
    if ((static_cast<int>(std::get<4>(reflockrecord).size()) > 6) ||
        (static_cast<int>(std::get<4>(reflockrecord).size()) < 2))
    {
        BMCWEB_LOG_DEBUG << "Validation of Number of Segements Failed";
        BMCWEB_LOG_DEBUG << "Number of Segments provied : "
                         << sizeof(std::get<4>(reflockrecord));
        return false;
    }

    int lockflag = 0;
    // validate the lockflags & segment length

    for (const auto &p : std::get<4>(reflockrecord))
    {

        // validate the lock flags
        // Allowed lockflags are locksame,lockall & dontlock

        if (!((boost::equals(p.first, "LockSame") ||
               (boost::equals(p.first, "LockAll")) ||
               (boost::equals(p.first, "DontLock")))))
        {
            BMCWEB_LOG_DEBUG << "Validation of lock flags failed";
            BMCWEB_LOG_DEBUG << p.first;
            return false;
        }

        // validate the segment length
        // Allowed values of segment length are between 1 and 4

        if (p.second < 1 || p.second > 4)
        {
            BMCWEB_LOG_DEBUG << "Validation of Segment Length Failed";
            BMCWEB_LOG_DEBUG << p.second;
            return false;
        }

        if ((boost::equals(p.first, "LockSame") ||
             (boost::equals(p.first, "LockAll"))))
        {
            ++lockflag;
            if (lockflag >= 2)
            {
                return false;
            }
        }
    }

    // validate the segment length
    return true;
}

rc lock::isconflictwithtable(const lockrequests reflockrequeststructure)
{

    uint32_t transactionID;

    if (locktable.empty())
    {
        transactionID = generateTransactionID();
        BMCWEB_LOG_DEBUG << transactionID;
        // Lock table is empty, so we are safe to add the lockrecords
        // as there will be no conflict
        BMCWEB_LOG_DEBUG << "Lock table is empty, so adding the lockrecords";
        locktable.emplace(std::pair<uint32_t, lockrequests>(
            transactionID, reflockrequeststructure));

        return std::make_pair(false, transactionID);
    }

    else
    {
        BMCWEB_LOG_DEBUG
            << "Lock table is not empty, check for conflict with lock table";
        // Lock table is not empty, compare the lockrequest entries with
        // the entries in the lock table

        for (auto lockrecord1 : reflockrequeststructure)
        {
            for (const auto &map : locktable)
            {
                for (auto lockrecord2 : map.second)
                {
                    const lockrequest &p = lockrecord1;
                    const lockrequest &q = lockrecord2;
                    bool status = isconflictrecord(p, q);
                    if (status)
                    {
                        return std::make_pair(true,
                                              std::make_pair(map.first, q));
                    }
                }
            }
        }

        // Reached here, so no conflict with the locktable, so we are safe to
        // add the request records into the lock table

        // Lock table is empty, so we are safe to add the lockrecords
        // as there will be no conflict
        BMCWEB_LOG_DEBUG << " Adding elements into lock table";
        transactionID = generateTransactionID();
        locktable.emplace(
            std::make_pair(transactionID, reflockrequeststructure));
    }
    return std::make_pair(false, transactionID);
}

bool lock::isconflictrequest(const lockrequests reflockrequeststructure)
{
    // check for all the locks coming in as a part of single request
    // return conflict if any two lock requests are conflicting

    if (reflockrequeststructure.size() == 1)
    {
        BMCWEB_LOG_DEBUG << "Only single lock request, so there is no conflict";
        // This means , we have only one lock request in the current
        // request , so no conflict within the request
        return false;
    }

    else
    {
        BMCWEB_LOG_DEBUG
            << "There are multiple lock requests coming in a single request";

        // There are multiple requests a part of one request

        for (uint32_t i = 0; i < reflockrequeststructure.size(); i++)
        {
            for (uint32_t j = i + 1; j < reflockrequeststructure.size(); j++)
            {
                const lockrequest &p = reflockrequeststructure[i];
                const lockrequest &q = reflockrequeststructure[j];
                bool status = isconflictrecord(p, q);

                if (status)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// This function converts the provided uint64_t resource id's from the two
// lock requests subjected for comparision, and this function also compares
// the content by bytes mentioned by a uint32_t number.

// If all the elements in the lock requests which are subjected for comparison
// are same, then the last comparision would be to check for the respective
// bytes in the resourceid based on the segment length.

bool lock::checkbyte(uint64_t resourceid1, uint64_t resourceid2, uint32_t j)
{
    uint8_t *p = reinterpret_cast<uint8_t *>(&resourceid1);
    uint8_t *q = reinterpret_cast<uint8_t *>(&resourceid2);

    BMCWEB_LOG_DEBUG << "Comparing bytes " << std::to_string(p[j]) << ","
                     << std::to_string(q[j]);
    if (p[j] != q[j])
    {
        return false;
    }

    else
    {
        return true;
    }
    return true;
}

bool lock::isconflictrecord(const lockrequest reflockrecord1,
                            const lockrequest reflockrecord2)
{
    // No conflict if both are read locks

    if (boost::equals(std::get<2>(reflockrecord1), "Read") &&
        boost::equals(std::get<2>(reflockrecord2), "Read"))
    {
        BMCWEB_LOG_DEBUG << "Both are read locks, no conflict";
        return false;
    }

    else
    {
        uint32_t i = 0;
        for (const auto &p : std::get<4>(reflockrecord1))
        {

            // return conflict when any of them is try to lock all resources
            // under the current resource level.
            if (boost::equals(p.first, "LockAll") ||
                boost::equals(std::get<4>(reflockrecord2)[i].first, "LockAll"))
            {
                BMCWEB_LOG_DEBUG
                    << "Either of the Comparing locks are trying to lock all "
                       "resources under the current resource level";
                return true;
            }

            // determine if there is a lock-all-with-same-segment-size.
            // If the current segment sizes are the same,then we should fail.

            if ((boost::equals(p.first, "LockSame") ||
                 boost::equals(std::get<4>(reflockrecord2)[i].first,
                               "LockSame")) &&
                (p.second == std::get<4>(reflockrecord2)[i].second))
            {
                return true;
            }

            // if segment lengths are not the same, it means two different locks
            // So no conflict
            if (p.second != std::get<4>(reflockrecord2)[i].second)
            {
                BMCWEB_LOG_DEBUG << "Segment lengths are not same";
                BMCWEB_LOG_DEBUG << "Segment 1 length : " << p.second;
                BMCWEB_LOG_DEBUG << "Segment 2 length : "
                                 << std::get<4>(reflockrecord2)[i].second;
                return false;
            }

            // compare segment data

            for (uint32_t i = 0; i < p.second; i++)
            {
                // if the segment data is different , then the locks is on a
                // different resource So no conflict between the lock records
                if (!(checkbyte(std::get<3>(reflockrecord1),
                                std::get<3>(reflockrecord2), i)))
                {
                    return false;
                }
            }

            ++i;
        }
    }

    return false;
}

uint32_t lock::generateTransactionID()
{
    ++transactionID;
    return transactionID;
}

} // namespace ibm_mc_lock
} // namespace crow
