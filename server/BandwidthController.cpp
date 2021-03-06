/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0

/*
 * The CommandListener, FrameworkListener don't allow for
 * multiple calls in parallel to reach the BandwidthController.
 * If they ever were to allow it, then netd/ would need some tweaking.
 */

#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>

#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#define LOG_TAG "BandwidthController"
#include <cutils/log.h>
#include <cutils/properties.h>
#include <logwrap/logwrap.h>

#include "NetdConstants.h"
#include "BandwidthController.h"
#include "NatController.h"  /* For LOCAL_TETHER_COUNTERS_CHAIN */
#include "ResponseCode.h"
#include "QtiConnectivityAdapter.h"

/* Alphabetical */
#define ALERT_IPT_TEMPLATE "%s %s -m quota2 ! --quota %" PRId64" --name %s\n"
const char* BandwidthController::LOCAL_INPUT = "bw_INPUT";
const char* BandwidthController::LOCAL_FORWARD = "bw_FORWARD";
const char* BandwidthController::LOCAL_OUTPUT = "bw_OUTPUT";
const char* BandwidthController::LOCAL_RAW_PREROUTING = "bw_raw_PREROUTING";
const char* BandwidthController::LOCAL_MANGLE_POSTROUTING = "bw_mangle_POSTROUTING";

auto BandwidthController::execFunction = android_fork_execvp;
auto BandwidthController::popenFunction = popen;
auto BandwidthController::iptablesRestoreFunction = execIptablesRestoreWithOutput;

using android::base::StringAppendF;
using android::base::StringPrintf;

namespace {

const char ALERT_GLOBAL_NAME[] = "globalAlert";
const int  MAX_CMD_ARGS = 32;
const int  MAX_CMD_LEN = 1024;
const int  MAX_IFACENAME_LEN = 64;
const int  MAX_IPT_OUTPUT_LINE_LEN = 256;
const std::string NEW_CHAIN_COMMAND = "-N ";
const std::string GET_TETHER_STATS_COMMAND = StringPrintf(
    "*filter\n"
    "-nvx -L %s\n"
    "COMMIT\n", NatController::LOCAL_TETHER_COUNTERS_CHAIN);


/**
 * Some comments about the rules:
 *  * Ordering
 *    - when an interface is marked as costly it should be INSERTED into the INPUT/OUTPUT chains.
 *      E.g. "-I bw_INPUT -i rmnet0 --jump costly"
 *    - quota'd rules in the costly chain should be before bw_penalty_box lookups.
 *    - the qtaguid counting is done at the end of the bw_INPUT/bw_OUTPUT user chains.
 *
 * * global quota vs per interface quota
 *   - global quota for all costly interfaces uses a single costly chain:
 *    . initial rules
 *      iptables -N bw_costly_shared
 *      iptables -I bw_INPUT -i iface0 --jump bw_costly_shared
 *      iptables -I bw_OUTPUT -o iface0 --jump bw_costly_shared
 *      iptables -I bw_costly_shared -m quota \! --quota 500000 \
 *          --jump REJECT --reject-with icmp-net-prohibited
 *      iptables -A bw_costly_shared --jump bw_penalty_box
 *      iptables -A bw_penalty_box --jump bw_happy_box
 *      iptables -A bw_happy_box --jump bw_data_saver
 *
 *    . adding a new iface to this, E.g.:
 *      iptables -I bw_INPUT -i iface1 --jump bw_costly_shared
 *      iptables -I bw_OUTPUT -o iface1 --jump bw_costly_shared
 *
 *   - quota per interface. This is achieve by having "costly" chains per quota.
 *     E.g. adding a new costly interface iface0 with its own quota:
 *      iptables -N bw_costly_iface0
 *      iptables -I bw_INPUT -i iface0 --jump bw_costly_iface0
 *      iptables -I bw_OUTPUT -o iface0 --jump bw_costly_iface0
 *      iptables -A bw_costly_iface0 -m quota \! --quota 500000 \
 *          --jump REJECT --reject-with icmp-port-unreachable
 *      iptables -A bw_costly_iface0 --jump bw_penalty_box
 *
 * * Penalty box, happy box and data saver.
 *   - bw_penalty box is a blacklist of apps that are rejected.
 *   - bw_happy_box is a whitelist of apps. It always includes all system apps
 *   - bw_data_saver implements data usage restrictions.
 *   - Via the UI the user can add and remove apps from the whitelist and
 *     blacklist, and turn on/off data saver.
 *   - The blacklist takes precedence over the whitelist and the whitelist
 *     takes precedence over data saver.
 *
 * * bw_penalty_box handling:
 *  - only one bw_penalty_box for all interfaces
 *   E.g  Adding an app:
 *    iptables -I bw_penalty_box -m owner --uid-owner app_3 \
 *        --jump REJECT --reject-with icmp-port-unreachable
 *
 * * bw_happy_box handling:
 *  - The bw_happy_box comes after the penalty box.
 *   E.g  Adding a happy app,
 *    iptables -I bw_happy_box -m owner --uid-owner app_3 \
 *        --jump RETURN
 *
 * * bw_data_saver handling:
 *  - The bw_data_saver comes after the happy box.
 *    Enable data saver:
 *      iptables -R 1 bw_data_saver --jump REJECT --reject-with icmp-port-unreachable
 *    Disable data saver:
 *      iptables -R 1 bw_data_saver --jump RETURN
 */

const std::string COMMIT_AND_CLOSE = "COMMIT\n";
const std::string HAPPY_BOX_WHITELIST_COMMAND = StringPrintf(
    "-I bw_happy_box -m owner --uid-owner %d-%d --jump RETURN", 0, MAX_SYSTEM_UID);

static const std::vector<std::string> IPT_FLUSH_COMMANDS = {
    /*
     * Cleanup rules.
     * Should normally include bw_costly_<iface>, but we rely on the way they are setup
     * to allow coexistance.
     */
    "*filter",
    ":bw_INPUT -",
    ":bw_OUTPUT -",
    ":bw_FORWARD -",
    ":bw_happy_box -",
    ":bw_penalty_box -",
    ":bw_data_saver -",
    ":bw_costly_shared -",
    "COMMIT",
    "*raw",
    ":bw_raw_PREROUTING -",
    "COMMIT",
    "*mangle",
    ":bw_mangle_POSTROUTING -",
    COMMIT_AND_CLOSE
};

static const std::vector<std::string> IPT_BASIC_ACCOUNTING_COMMANDS = {
    "*filter",
    "-A bw_INPUT -m owner --socket-exists", /* This is a tracking rule. */
    "-A bw_OUTPUT -m owner --socket-exists", /* This is a tracking rule. */
    "-A bw_costly_shared --jump bw_penalty_box",
    "-A bw_penalty_box --jump bw_happy_box",
    "-A bw_happy_box --jump bw_data_saver",
    "-A bw_data_saver -j RETURN",
    HAPPY_BOX_WHITELIST_COMMAND,
    "COMMIT",

    "*raw",
    "-A bw_raw_PREROUTING -m owner --socket-exists", /* This is a tracking rule. */
    "COMMIT",

    "*mangle",
    "-A bw_mangle_POSTROUTING -m owner --socket-exists", /* This is a tracking rule. */
    COMMIT_AND_CLOSE
};


}  // namespace

BandwidthController::BandwidthController(void) {
}

int BandwidthController::runIpxtablesCmd(const char *cmd, IptJumpOp jumpHandling,
                                         IptFailureLog failureHandling) {
    int res = 0;

    ALOGV("runIpxtablesCmd(cmd=%s)", cmd);
    res |= runIptablesCmd(cmd, jumpHandling, IptIpV4, failureHandling);
    res |= runIptablesCmd(cmd, jumpHandling, IptIpV6, failureHandling);
    return res;
}

int BandwidthController::StrncpyAndCheck(char *buffer, const char *src, size_t buffSize) {

    memset(buffer, '\0', buffSize);  // strncpy() is not filling leftover with '\0'
    strncpy(buffer, src, buffSize);
    return buffer[buffSize - 1];
}

int BandwidthController::runIptablesCmd(const char *cmd, IptJumpOp jumpHandling,
                                        IptIpVer iptVer, IptFailureLog failureHandling) {
    char buffer[MAX_CMD_LEN];
    const char *argv[MAX_CMD_ARGS];
    int argc = 0;
    char *next = buffer;
    char *tmp;
    int res;
    int status = 0;

    std::string fullCmd = cmd;
    fullCmd += jumpToString(jumpHandling);

    fullCmd.insert(0, " -w ");
    fullCmd.insert(0, iptVer == IptIpV4 ? IPTABLES_PATH : IP6TABLES_PATH);

    if (StrncpyAndCheck(buffer, fullCmd.c_str(), sizeof(buffer))) {
        ALOGE("iptables command too long");
        return -1;
    }

    argc = 0;
    while ((tmp = strsep(&next, " "))) {
        argv[argc++] = tmp;
        if (argc >= MAX_CMD_ARGS) {
            ALOGE("iptables argument overflow");
            return -1;
        }
    }

    argv[argc] = NULL;
    res = execFunction(argc, (char **)argv, &status, false,
            failureHandling == IptFailShow);
    res = res || !WIFEXITED(status) || WEXITSTATUS(status);
    if (res && failureHandling == IptFailShow) {
      ALOGE("runIptablesCmd(): res=%d status=%d failed %s", res, status,
            fullCmd.c_str());
    }
    return res;
}

void BandwidthController::flushCleanTables(bool doClean) {
    /* Flush and remove the bw_costly_<iface> tables */
    flushExistingCostlyTables(doClean);

    std::string commands = android::base::Join(IPT_FLUSH_COMMANDS, '\n');
    iptablesRestoreFunction(V4V6, commands, nullptr);
}

int BandwidthController::setupIptablesHooks(void) {
    /* flush+clean is allowed to fail */
    flushCleanTables(true);
    return 0;
}

int BandwidthController::enableBandwidthControl(bool force) {
    char value[PROPERTY_VALUE_MAX];

    if (!force) {
            property_get("persist.bandwidth.enable", value, "1");
            if (!strcmp(value, "0"))
                    return 0;
    }

    /* Let's pretend we started from scratch ... */
    sharedQuotaIfaces.clear();
    quotaIfaces.clear();
    globalAlertBytes = 0;
    globalAlertTetherCount = 0;
    sharedQuotaBytes = sharedAlertBytes = 0;

    flushCleanTables(false);
    std::string commands = android::base::Join(IPT_BASIC_ACCOUNTING_COMMANDS, '\n');
    return iptablesRestoreFunction(V4V6, commands, nullptr);
}

int BandwidthController::disableBandwidthControl(void) {

    flushCleanTables(false);
    return 0;
}

int BandwidthController::enableDataSaver(bool enable) {
    std::string cmd = StringPrintf(
        "*filter\n"
        "-R bw_data_saver 1%s\n"
        "COMMIT\n", jumpToString(enable ? IptJumpReject : IptJumpReturn));
    return iptablesRestoreFunction(V4V6, cmd, nullptr);
}

int BandwidthController::runCommands(int numCommands, const char *commands[],
                                     RunCmdErrHandling cmdErrHandling) {
    int res = 0;
    IptFailureLog failureLogging = IptFailShow;
    if (cmdErrHandling == RunCmdFailureOk) {
        failureLogging = IptFailHide;
    }
    ALOGV("runCommands(): %d commands", numCommands);
    for (int cmdNum = 0; cmdNum < numCommands; cmdNum++) {
        res = runIpxtablesCmd(commands[cmdNum], IptJumpNoAdd, failureLogging);
        if (res && cmdErrHandling != RunCmdFailureOk)
            return res;
    }
    return 0;
}

int BandwidthController::addNaughtyApps(int numUids, char *appUids[]) {
    return manipulateNaughtyApps(numUids, appUids, IptOpInsert);
}

int BandwidthController::removeNaughtyApps(int numUids, char *appUids[]) {
    return manipulateNaughtyApps(numUids, appUids, IptOpDelete);
}

int BandwidthController::addNiceApps(int numUids, char *appUids[]) {
    return manipulateNiceApps(numUids, appUids, IptOpInsert);
}

int BandwidthController::removeNiceApps(int numUids, char *appUids[]) {
    return manipulateNiceApps(numUids, appUids, IptOpDelete);
}

int BandwidthController::manipulateNaughtyApps(int numUids, char *appStrUids[], IptOp op) {
    return manipulateSpecialApps(numUids, appStrUids, "bw_penalty_box", IptJumpReject, op);
}

int BandwidthController::manipulateNiceApps(int numUids, char *appStrUids[], IptOp op) {
    return manipulateSpecialApps(numUids, appStrUids, "bw_happy_box", IptJumpReturn, op);
}

int BandwidthController::manipulateSpecialApps(int numUids, char *appStrUids[],
                                               const char *chain,
                                               IptJumpOp jumpHandling, IptOp op) {
    std::string cmd = "*filter\n";
    for (int uidNum = 0; uidNum < numUids; uidNum++) {
        StringAppendF(&cmd, "%s %s -m owner --uid-owner %s%s\n", opToString(op), chain,
                      appStrUids[uidNum], jumpToString(jumpHandling));
    }
    StringAppendF(&cmd, "COMMIT\n");
    return iptablesRestoreFunction(V4V6, cmd, nullptr);
}

std::string BandwidthController::makeIptablesQuotaCmd(IptFullOp op, const char *costName, int64_t quota) {
    std::string res;
    char *buff;
    const char *opFlag;

    ALOGV("makeIptablesQuotaCmd(%d, %" PRId64")", op, quota);

    switch (op) {
    case IptFullOpInsert:
        opFlag = "-I";
        break;
    case IptFullOpAppend:
        opFlag = "-A";
        break;
    case IptFullOpDelete:
        opFlag = "-D";
        break;
    }

    // The requried IP version specific --jump REJECT ... will be added later.
    asprintf(&buff, "%s bw_costly_%s -m quota2 ! --quota %" PRId64" --name %s", opFlag, costName, quota,
             costName);
    res = buff;
    free(buff);
    return res;
}

int BandwidthController::prepCostlyIface(const char *ifn, QuotaType quotaType) {
    char cmd[MAX_CMD_LEN];
    int res = 0, res1, res2;
    int ruleInsertPos = 1;
    std::string costString;
    const char *costCString;

    /* The "-N costly" is created upfront, no need to handle it here. */
    switch (quotaType) {
    case QuotaUnique:
        costString = "bw_costly_";
        costString += ifn;
        costCString = costString.c_str();
        /*
         * Flush the bw_costly_<iface> is allowed to fail in case it didn't exist.
         * Creating a new one is allowed to fail in case it existed.
         * This helps with netd restarts.
         */
        snprintf(cmd, sizeof(cmd), "-F %s", costCString);
        res1 = runIpxtablesCmd(cmd, IptJumpNoAdd, IptFailHide);
        snprintf(cmd, sizeof(cmd), "-N %s", costCString);
        res2 = runIpxtablesCmd(cmd, IptJumpNoAdd, IptFailHide);
        res = (res1 && res2) || (!res1 && !res2);

        snprintf(cmd, sizeof(cmd), "-A %s -j bw_penalty_box", costCString);
        res |= runIpxtablesCmd(cmd, IptJumpNoAdd);
        break;
    case QuotaShared:
        costCString = "bw_costly_shared";
        break;
    }

    if (globalAlertBytes) {
        /* The alert rule comes 1st */
        ruleInsertPos = 2;
    }

    snprintf(cmd, sizeof(cmd), "-D bw_INPUT -i %s --jump %s", ifn, costCString);
    runIpxtablesCmd(cmd, IptJumpNoAdd, IptFailHide);

    snprintf(cmd, sizeof(cmd), "-I bw_INPUT %d -i %s --jump %s", ruleInsertPos, ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptJumpNoAdd);

    snprintf(cmd, sizeof(cmd), "-D bw_OUTPUT -o %s --jump %s", ifn, costCString);
    runIpxtablesCmd(cmd, IptJumpNoAdd, IptFailHide);

    snprintf(cmd, sizeof(cmd), "-I bw_OUTPUT %d -o %s --jump %s", ruleInsertPos, ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptJumpNoAdd);

    snprintf(cmd, sizeof(cmd), "-D bw_FORWARD -o %s --jump %s", ifn, costCString);
    runIpxtablesCmd(cmd, IptJumpNoAdd, IptFailHide);
    snprintf(cmd, sizeof(cmd), "-A bw_FORWARD -o %s --jump %s", ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptJumpNoAdd);

    return res;
}

int BandwidthController::cleanupCostlyIface(const char *ifn, QuotaType quotaType) {
    char cmd[MAX_CMD_LEN];
    int res = 0;
    std::string costString;
    const char *costCString;

    switch (quotaType) {
    case QuotaUnique:
        costString = "bw_costly_";
        costString += ifn;
        costCString = costString.c_str();
        break;
    case QuotaShared:
        costCString = "bw_costly_shared";
        break;
    }

    snprintf(cmd, sizeof(cmd), "-D bw_INPUT -i %s --jump %s", ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptJumpNoAdd);
    for (const auto tableName : {LOCAL_OUTPUT, LOCAL_FORWARD}) {
        snprintf(cmd, sizeof(cmd), "-D %s -o %s --jump %s", tableName, ifn, costCString);
        res |= runIpxtablesCmd(cmd, IptJumpNoAdd);
    }

    /* The "-N bw_costly_shared" is created upfront, no need to handle it here. */
    if (quotaType == QuotaUnique) {
        snprintf(cmd, sizeof(cmd), "-F %s", costCString);
        res |= runIpxtablesCmd(cmd, IptJumpNoAdd);
        snprintf(cmd, sizeof(cmd), "-X %s", costCString);
        res |= runIpxtablesCmd(cmd, IptJumpNoAdd);
    }
    return res;
}

int BandwidthController::setInterfaceSharedQuota(const char *iface, int64_t maxBytes) {
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string quotaCmd;
    std::string ifaceName;
    ;
    const char *costName = "shared";
    std::list<std::string>::iterator it;

    if (!maxBytes) {
        /* Don't talk about -1, deprecate it. */
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    if (!isIfaceName(iface))
        return -1;
    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;

    if (maxBytes == -1) {
        return removeInterfaceSharedQuota(ifn);
    }

    /* Insert ingress quota. */
    for (it = sharedQuotaIfaces.begin(); it != sharedQuotaIfaces.end(); it++) {
        if (*it == ifaceName)
            break;
    }

    if (it == sharedQuotaIfaces.end()) {
        res |= prepCostlyIface(ifn, QuotaShared);
        if (sharedQuotaIfaces.empty()) {
            quotaCmd = makeIptablesQuotaCmd(IptFullOpInsert, costName, maxBytes);
            res |= runIpxtablesCmd(quotaCmd.c_str(), IptJumpReject);
            if (res) {
                ALOGE("Failed set quota rule");
                goto fail;
            }
            sharedQuotaBytes = maxBytes;
        }
        sharedQuotaIfaces.push_front(ifaceName);

    }

    if (maxBytes != sharedQuotaBytes) {
        res |= updateQuota(costName, maxBytes);
        if (res) {
            ALOGE("Failed update quota for %s", costName);
            goto fail;
        }
        sharedQuotaBytes = maxBytes;
    }
    return 0;

    fail:
    /*
     * TODO(jpa): once we get rid of iptables in favor of rtnetlink, reparse
     * rules in the kernel to see which ones need cleaning up.
     * For now callers needs to choose if they want to "ndc bandwidth enable"
     * which resets everything.
     */
    removeInterfaceSharedQuota(ifn);
    return -1;
}

/* It will also cleanup any shared alerts */
int BandwidthController::removeInterfaceSharedQuota(const char *iface) {
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string ifaceName;
    std::list<std::string>::iterator it;
    const char *costName = "shared";

    if (!isIfaceName(iface))
        return -1;
    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;

    for (it = sharedQuotaIfaces.begin(); it != sharedQuotaIfaces.end(); it++) {
        if (*it == ifaceName)
            break;
    }
    if (it == sharedQuotaIfaces.end()) {
        ALOGE("No such iface %s to delete", ifn);
        return -1;
    }

    res |= cleanupCostlyIface(ifn, QuotaShared);
    sharedQuotaIfaces.erase(it);

    if (sharedQuotaIfaces.empty()) {
        std::string quotaCmd;
        quotaCmd = makeIptablesQuotaCmd(IptFullOpDelete, costName, sharedQuotaBytes);
        res |= runIpxtablesCmd(quotaCmd.c_str(), IptJumpReject);
        sharedQuotaBytes = 0;
        if (sharedAlertBytes) {
            removeSharedAlert();
            sharedAlertBytes = 0;
        }
    }
    return res;
}

int BandwidthController::setInterfaceQuota(const char *iface, int64_t maxBytes) {
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string ifaceName;
    const char *costName;
    std::list<QuotaInfo>::iterator it;
    std::string quotaCmd;

    if (!isIfaceName(iface))
        return -1;

    if (!maxBytes) {
        /* Don't talk about -1, deprecate it. */
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    if (maxBytes == -1) {
        return removeInterfaceQuota(iface);
    }

    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;
    costName = iface;

    /* Insert ingress quota. */
    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == ifaceName)
            break;
    }

    if (it == quotaIfaces.end()) {
        /* Preparing the iface adds a penalty/happy box check */
        res |= prepCostlyIface(ifn, QuotaUnique);
        /*
         * The rejecting quota limit should go after the penalty/happy box checks
         * or else a naughty app could just eat up the quota.
         * So we append here.
         */
        quotaCmd = makeIptablesQuotaCmd(IptFullOpAppend, costName, maxBytes);
        res |= runIpxtablesCmd(quotaCmd.c_str(), IptJumpReject);
        if (res) {
            ALOGE("Failed set quota rule");
            goto fail;
        }

        quotaIfaces.push_front(QuotaInfo(ifaceName, maxBytes, 0));

    } else {
        res |= updateQuota(costName, maxBytes);
        if (res) {
            ALOGE("Failed update quota for %s", iface);
            goto fail;
        }
        it->quota = maxBytes;
    }
    return 0;

    fail:
    /*
     * TODO(jpa): once we get rid of iptables in favor of rtnetlink, reparse
     * rules in the kernel to see which ones need cleaning up.
     * For now callers needs to choose if they want to "ndc bandwidth enable"
     * which resets everything.
     */
    removeInterfaceSharedQuota(ifn);
    return -1;
}

int BandwidthController::getInterfaceSharedQuota(int64_t *bytes) {
    return getInterfaceQuota("shared", bytes);
}

int BandwidthController::getInterfaceQuota(const char *costName, int64_t *bytes) {
    FILE *fp;
    char *fname;
    int scanRes;

    if (!isIfaceName(costName))
        return -1;

    asprintf(&fname, "/proc/net/xt_quota/%s", costName);
    fp = fopen(fname, "re");
    free(fname);
    if (!fp) {
        ALOGE("Reading quota %s failed (%s)", costName, strerror(errno));
        return -1;
    }
    scanRes = fscanf(fp, "%" SCNd64, bytes);
    ALOGV("Read quota res=%d bytes=%" PRId64, scanRes, *bytes);
    fclose(fp);
    return scanRes == 1 ? 0 : -1;
}

int BandwidthController::removeInterfaceQuota(const char *iface) {

    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string ifaceName;
    std::list<QuotaInfo>::iterator it;

    if (!isIfaceName(iface))
        return -1;
    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;

    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == ifaceName)
            break;
    }

    if (it == quotaIfaces.end()) {
        ALOGE("No such iface %s to delete", ifn);
        return -1;
    }

    /* This also removes the quota command of CostlyIface chain. */
    res |= cleanupCostlyIface(ifn, QuotaUnique);

    quotaIfaces.erase(it);

    return res;
}

int BandwidthController::updateQuota(const char *quotaName, int64_t bytes) {
    FILE *fp;
    char *fname;

    if (!isIfaceName(quotaName)) {
        ALOGE("updateQuota: Invalid quotaName \"%s\"", quotaName);
        return -1;
    }

    asprintf(&fname, "/proc/net/xt_quota/%s", quotaName);
    fp = fopen(fname, "we");
    free(fname);
    if (!fp) {
        ALOGE("Updating quota %s failed (%s)", quotaName, strerror(errno));
        return -1;
    }
    fprintf(fp, "%" PRId64"\n", bytes);
    fclose(fp);
    return 0;
}

int BandwidthController::runIptablesAlertCmd(IptOp op, const char *alertName, int64_t bytes) {
    const char *opFlag = opToString(op);
    std::string alertQuotaCmd = "*filter\n";

    // TODO: consider using an alternate template for the delete that does not include the --quota
    // value. This code works because the --quota value is ignored by deletes
    StringAppendF(&alertQuotaCmd, ALERT_IPT_TEMPLATE, opFlag, "bw_INPUT", bytes, alertName);
    StringAppendF(&alertQuotaCmd, ALERT_IPT_TEMPLATE, opFlag, "bw_OUTPUT", bytes, alertName);
    StringAppendF(&alertQuotaCmd, "COMMIT\n");

    return iptablesRestoreFunction(V4V6, alertQuotaCmd, nullptr);
}

int BandwidthController::runIptablesAlertFwdCmd(IptOp op, const char *alertName, int64_t bytes) {
    const char *opFlag = opToString(op);
    std::string alertQuotaCmd = "*filter\n";
    StringAppendF(&alertQuotaCmd, ALERT_IPT_TEMPLATE, opFlag, "bw_FORWARD", bytes, alertName);
    StringAppendF(&alertQuotaCmd, "COMMIT\n");

    return iptablesRestoreFunction(V4V6, alertQuotaCmd, nullptr);
}

int BandwidthController::setGlobalAlert(int64_t bytes) {
    const char *alertName = ALERT_GLOBAL_NAME;
    int res = 0;

    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    if (globalAlertBytes) {
        res = updateQuota(alertName, bytes);
    } else {
        res = runIptablesAlertCmd(IptOpInsert, alertName, bytes);
        if (globalAlertTetherCount) {
            ALOGV("setGlobalAlert for %d tether", globalAlertTetherCount);
            res |= runIptablesAlertFwdCmd(IptOpInsert, alertName, bytes);
        }
    }
    globalAlertBytes = bytes;
    return res;
}

int BandwidthController::setGlobalAlertInForwardChain(void) {
    const char *alertName = ALERT_GLOBAL_NAME;
    int res = 0;

    globalAlertTetherCount++;
    ALOGV("setGlobalAlertInForwardChain(): %d tether", globalAlertTetherCount);

    /*
     * If there is no globalAlert active we are done.
     * If there is an active globalAlert but this is not the 1st
     * tether, we are also done.
     */
    if (!globalAlertBytes || globalAlertTetherCount != 1) {
        return 0;
    }

    /* We only add the rule if this was the 1st tether added. */
    res = runIptablesAlertFwdCmd(IptOpInsert, alertName, globalAlertBytes);
    return res;
}

int BandwidthController::removeGlobalAlert(void) {

    const char *alertName = ALERT_GLOBAL_NAME;
    int res = 0;

    if (!globalAlertBytes) {
        ALOGE("No prior alert set");
        return -1;
    }
    res = runIptablesAlertCmd(IptOpDelete, alertName, globalAlertBytes);
    if (globalAlertTetherCount) {
        res |= runIptablesAlertFwdCmd(IptOpDelete, alertName, globalAlertBytes);
    }
    globalAlertBytes = 0;
    return res;
}

int BandwidthController::removeGlobalAlertInForwardChain(void) {
    int res = 0;
    const char *alertName = ALERT_GLOBAL_NAME;

    if (!globalAlertTetherCount) {
        ALOGE("No prior alert set");
        return -1;
    }

    globalAlertTetherCount--;
    /*
     * If there is no globalAlert active we are done.
     * If there is an active globalAlert but there are more
     * tethers, we are also done.
     */
    if (!globalAlertBytes || globalAlertTetherCount >= 1) {
        return 0;
    }

    /* We only detete the rule if this was the last tether removed. */
    res = runIptablesAlertFwdCmd(IptOpDelete, alertName, globalAlertBytes);
    return res;
}

int BandwidthController::setSharedAlert(int64_t bytes) {
    if (!sharedQuotaBytes) {
        ALOGE("Need to have a prior shared quota set to set an alert");
        return -1;
    }
    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    return setCostlyAlert("shared", bytes, &sharedAlertBytes);
}

int BandwidthController::removeSharedAlert(void) {
    return removeCostlyAlert("shared", &sharedAlertBytes);
}

int BandwidthController::setInterfaceAlert(const char *iface, int64_t bytes) {
    std::list<QuotaInfo>::iterator it;

    if (!isIfaceName(iface)) {
        ALOGE("setInterfaceAlert: Invalid iface \"%s\"", iface);
        return -1;
    }

    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == iface)
            break;
    }

    if (it == quotaIfaces.end()) {
        ALOGE("Need to have a prior interface quota set to set an alert");
        return -1;
    }

    return setCostlyAlert(iface, bytes, &it->alert);
}

int BandwidthController::removeInterfaceAlert(const char *iface) {
    std::list<QuotaInfo>::iterator it;

    if (!isIfaceName(iface)) {
        ALOGE("removeInterfaceAlert: Invalid iface \"%s\"", iface);
        return -1;
    }

    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == iface)
            break;
    }

    if (it == quotaIfaces.end()) {
        ALOGE("No prior alert set for interface %s", iface);
        return -1;
    }

    return removeCostlyAlert(iface, &it->alert);
}

int BandwidthController::setCostlyAlert(const char *costName, int64_t bytes, int64_t *alertBytes) {
    char *alertQuotaCmd;
    char *chainName;
    int res = 0;
    char *alertName;

    if (!isIfaceName(costName)) {
        ALOGE("setCostlyAlert: Invalid costName \"%s\"", costName);
        return -1;
    }

    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    asprintf(&alertName, "%sAlert", costName);
    if (*alertBytes) {
        res = updateQuota(alertName, *alertBytes);
    } else {
        asprintf(&chainName, "bw_costly_%s", costName);
        asprintf(&alertQuotaCmd, ALERT_IPT_TEMPLATE, "-A", chainName, bytes, alertName);
        res |= runIpxtablesCmd(alertQuotaCmd, IptJumpNoAdd);
        free(alertQuotaCmd);
        free(chainName);
    }
    *alertBytes = bytes;
    free(alertName);
    return res;
}

int BandwidthController::removeCostlyAlert(const char *costName, int64_t *alertBytes) {
    char *alertQuotaCmd;
    char *chainName;
    char *alertName;
    int res = 0;

    if (!isIfaceName(costName)) {
        ALOGE("removeCostlyAlert: Invalid costName \"%s\"", costName);
        return -1;
    }

    if (!*alertBytes) {
        ALOGE("No prior alert set for %s alert", costName);
        return -1;
    }

    asprintf(&alertName, "%sAlert", costName);
    asprintf(&chainName, "bw_costly_%s", costName);
    asprintf(&alertQuotaCmd, ALERT_IPT_TEMPLATE, "-D", chainName, *alertBytes, alertName);
    res |= runIpxtablesCmd(alertQuotaCmd, IptJumpNoAdd);
    free(alertQuotaCmd);
    free(chainName);

    *alertBytes = 0;
    free(alertName);
    return res;
}

void BandwidthController::addStats(TetherStatsList& statsList, const TetherStats& stats) {
    for (TetherStats& existing : statsList) {
        if (existing.addStatsIfMatch(stats)) {
            return;
        }
    }
    // No match. Insert a new interface pair.
    statsList.push_back(stats);
}

/*
 * Parse the ptks and bytes out of:
 *   Chain natctrl_tether_counters (4 references)
 *       pkts      bytes target     prot opt in     out     source               destination
 *         26     2373 RETURN     all  --  wlan0  rmnet0  0.0.0.0/0            0.0.0.0/0
 *         27     2002 RETURN     all  --  rmnet0 wlan0   0.0.0.0/0            0.0.0.0/0
 *       1040   107471 RETURN     all  --  bt-pan rmnet0  0.0.0.0/0            0.0.0.0/0
 *       1450  1708806 RETURN     all  --  rmnet0 bt-pan  0.0.0.0/0            0.0.0.0/0
 * or:
 *   Chain natctrl_tether_counters (0 references)
 *       pkts      bytes target     prot opt in     out     source               destination
 *          0        0 RETURN     all      wlan0  rmnet_data0  ::/0                 ::/0
 *          0        0 RETURN     all      rmnet_data0 wlan0   ::/0                 ::/0
 *
 * It results in an error if invoked and no tethering counter rules exist. The constraint
 * helps detect complete parsing failure.
 */
int BandwidthController::addForwardChainStats(const TetherStats& filter,
                                              TetherStatsList& statsList,
                                              const std::string& statsOutput,
                                              std::string &extraProcessingInfo) {
    int res;
    std::string statsLine;
    char iface0[MAX_IPT_OUTPUT_LINE_LEN];
    char iface1[MAX_IPT_OUTPUT_LINE_LEN];
    char rest[MAX_IPT_OUTPUT_LINE_LEN];

    TetherStats stats;
    const char *buffPtr;
    int64_t packets, bytes;
    int statsFound = 0;

    bool filterPair = filter.intIface[0] && filter.extIface[0];

    char *filterMsg = filter.getStatsLine();
    ALOGV("filter: %s",  filterMsg);
    free(filterMsg);

    stats = filter;

    std::stringstream stream(statsOutput);
    while (std::getline(stream, statsLine, '\n')) {
        buffPtr = statsLine.c_str();

        /* Clean up, so a failed parse can still print info */
        iface0[0] = iface1[0] = rest[0] = packets = bytes = 0;
        if (strstr(buffPtr, "0.0.0.0")) {
            // IPv4 has -- indicating what to do with fragments...
            //       26     2373 RETURN     all  --  wlan0  rmnet0  0.0.0.0/0            0.0.0.0/0
            res = sscanf(buffPtr, "%" SCNd64" %" SCNd64" RETURN all -- %s %s 0.%s",
                    &packets, &bytes, iface0, iface1, rest);
        } else {
            // ... but IPv6 does not.
            //       26     2373 RETURN     all      wlan0  rmnet0  ::/0                 ::/0
            res = sscanf(buffPtr, "%" SCNd64" %" SCNd64" RETURN all %s %s ::/%s",
                    &packets, &bytes, iface0, iface1, rest);
        }
        ALOGV("parse res=%d iface0=<%s> iface1=<%s> pkts=%" PRId64" bytes=%" PRId64" rest=<%s> orig line=<%s>", res,
             iface0, iface1, packets, bytes, rest, buffPtr);
        extraProcessingInfo += buffPtr;
        extraProcessingInfo += "\n";

        if (res != 5) {
            continue;
        }
        /*
         * The following assumes that the 1st rule has in:extIface out:intIface,
         * which is what NatController sets up.
         * If not filtering, the 1st match rx, and sets up the pair for the tx side.
         */
        if (filter.intIface[0] && filter.extIface[0]) {
            if (filter.intIface == iface0 && filter.extIface == iface1) {
                ALOGV("2Filter RX iface_in=%s iface_out=%s rx_bytes=%" PRId64" rx_packets=%" PRId64" ", iface0, iface1, bytes, packets);
                stats.rxPackets = packets;
                stats.rxBytes = bytes;
            } else if (filter.intIface == iface1 && filter.extIface == iface0) {
                ALOGV("2Filter TX iface_in=%s iface_out=%s rx_bytes=%" PRId64" rx_packets=%" PRId64" ", iface0, iface1, bytes, packets);
                stats.txPackets = packets;
                stats.txBytes = bytes;
            }
        } else if (filter.intIface[0] || filter.extIface[0]) {
            if (filter.intIface == iface0 || filter.extIface == iface1) {
                ALOGV("1Filter RX iface_in=%s iface_out=%s rx_bytes=%" PRId64" rx_packets=%" PRId64" ", iface0, iface1, bytes, packets);
                stats.intIface = iface0;
                stats.extIface = iface1;
                stats.rxPackets = packets;
                stats.rxBytes = bytes;
            } else if (filter.intIface == iface1 || filter.extIface == iface0) {
                ALOGV("1Filter TX iface_in=%s iface_out=%s rx_bytes=%" PRId64" rx_packets=%" PRId64" ", iface0, iface1, bytes, packets);
                stats.intIface = iface1;
                stats.extIface = iface0;
                stats.txPackets = packets;
                stats.txBytes = bytes;
            }
        } else /* if (!filter.intFace[0] && !filter.extIface[0]) */ {
            if (!stats.intIface[0]) {
                ALOGV("0Filter RX iface_in=%s iface_out=%s rx_bytes=%" PRId64" rx_packets=%" PRId64" ", iface0, iface1, bytes, packets);
                stats.intIface = iface0;
                stats.extIface = iface1;
                stats.rxPackets = packets;
                stats.rxBytes = bytes;
            } else if (stats.intIface == iface1 && stats.extIface == iface0) {
                ALOGV("0Filter TX iface_in=%s iface_out=%s rx_bytes=%" PRId64" rx_packets=%" PRId64" ", iface0, iface1, bytes, packets);
                stats.txPackets = packets;
                stats.txBytes = bytes;
            }
        }
        if (stats.rxBytes != -1 && stats.txBytes != -1) {
            ALOGV("rx_bytes=%" PRId64" tx_bytes=%" PRId64" filterPair=%d", stats.rxBytes, stats.txBytes, filterPair);
            addStats(statsList, stats);
            if (filterPair) {
                return 0;
            } else {
                statsFound++;
                stats = filter;
            }
        }
    }

    /* It is always an error to find only one side of the stats. */
    /* It is an error to find nothing when not filtering. */
    if (((stats.rxBytes == -1) != (stats.txBytes == -1)) ||
        (!statsFound && !filterPair)) {
        return -1;
    }
    return 0;
}

char *BandwidthController::TetherStats::getStatsLine(void) const {
    char *msg;
    asprintf(&msg, "%s %s %" PRId64" %" PRId64" %" PRId64" %" PRId64, intIface.c_str(), extIface.c_str(),
            rxBytes, rxPackets, txBytes, txPackets);
    return msg;
}

int BandwidthController::getTetherStats(SocketClient *cli, TetherStats& filter,
                                        std::string &extraProcessingInfo) {
    int res = 0;
    getV6TetherStats(cli, stats.intIface.c_str(), stats.extIface.c_str(), extraProcessingInfo);

    TetherStatsList statsList;

    for (const IptablesTarget target : {V4, V6}) {
        std::string statsString;
        res = iptablesRestoreFunction(target, GET_TETHER_STATS_COMMAND, &statsString);
        if (res != 0) {
            ALOGE("Failed to run %s err=%d", GET_TETHER_STATS_COMMAND.c_str(), res);
            return -1;
        }

        res = addForwardChainStats(filter, statsList, statsString, extraProcessingInfo);
        if (res != 0) {
            return res;
        }
    }

    if (filter.intIface[0] && filter.extIface[0] && statsList.size() == 1) {
        cli->sendMsg(ResponseCode::TetheringStatsResult, statsList[0].getStatsLine(), false);
    } else {
        for (const auto& stats: statsList) {
            cli->sendMsg(ResponseCode::TetheringStatsListResult, stats.getStatsLine(), false);
        }
        if (res == 0) {
            cli->sendMsg(ResponseCode::CommandOkay, "Tethering stats list completed", false);
        }
    }

    return res;
}

void BandwidthController::flushExistingCostlyTables(bool doClean) {
    std::string fullCmd = "*filter\n-S\nCOMMIT\n";
    std::string ruleList;

    /* Only lookup ip4 table names as ip6 will have the same tables ... */
    if (int ret = iptablesRestoreFunction(V4, fullCmd, &ruleList)) {
        ALOGE("Failed to list existing costly tables ret=%d", ret);
        return;
    }
    /* ... then flush/clean both ip4 and ip6 iptables. */
    parseAndFlushCostlyTables(ruleList, doClean);
}

void BandwidthController::parseAndFlushCostlyTables(const std::string& ruleList, bool doRemove) {
    std::stringstream stream(ruleList);
    std::string rule;
    std::vector<std::string> clearCommands = { "*filter" };
    std::string chainName;

    // Find and flush all rules starting with "-N bw_costly_<iface>" except "-N bw_costly_shared".
    while (std::getline(stream, rule, '\n')) {
        if (rule.find(NEW_CHAIN_COMMAND) != 0) continue;
        chainName = rule.substr(NEW_CHAIN_COMMAND.size());
        ALOGV("parse chainName=<%s> orig line=<%s>", chainName.c_str(), rule.c_str());

        if (chainName.find("bw_costly_") != 0 || chainName == std::string("bw_costly_shared")) {
            continue;
        }

        clearCommands.push_back(StringPrintf(":%s -", chainName.c_str()));
        if (doRemove) {
            clearCommands.push_back(StringPrintf("-X %s", chainName.c_str()));
        }
    }

    if (clearCommands.size() == 1) {
        // No rules found.
        return;
    }

    clearCommands.push_back("COMMIT\n");
    iptablesRestoreFunction(V4V6, android::base::Join(clearCommands, '\n'), nullptr);
}

inline const char *BandwidthController::opToString(IptOp op) {
    switch (op) {
    case IptOpInsert:
        return "-I";
    case IptOpDelete:
        return "-D";
    }
}

inline const char *BandwidthController::jumpToString(IptJumpOp jumpHandling) {
    /*
     * Must be careful what one rejects with, as upper layer protocols will just
     * keep on hammering the device until the number of retries are done.
     * For port-unreachable (default), TCP should consider as an abort (RFC1122).
     */
    switch (jumpHandling) {
    case IptJumpNoAdd:
        return "";
    case IptJumpReject:
        return " --jump REJECT";
    case IptJumpReturn:
        return " --jump RETURN";
    }
}
