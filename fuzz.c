/*
 *
 * honggfuzz - fuzzing routines
 * -----------------------------------------
 *
 * Author:
 * Robert Swiecki <swiecki@google.com>
 * Felix Gröbert <groebert@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "fuzz.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "arch.h"
#include "honggfuzz.h"
#include "input.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"
#include "mangle.h"
#include "report.h"
#include "sancov.h"
#include "sanitizers.h"
#include "subproc.h"

static time_t termTimeStamp = 0;

bool fuzz_isTerminating(void) {
    if (ATOMIC_GET(termTimeStamp) != 0) {
        return true;
    }
    return false;
}

void fuzz_setTerminating(void) {
    if (ATOMIC_GET(termTimeStamp) != 0) {
        return;
    }
    ATOMIC_SET(termTimeStamp, time(NULL));
}

bool fuzz_shouldTerminate() {
    if (ATOMIC_GET(termTimeStamp) == 0) {
        return false;
    }
    if ((time(NULL) - ATOMIC_GET(termTimeStamp)) > 5) {
        return true;
    }
    return false;
}

static bool fuzz_checkSizeNRewind(run_t* run) {
    if (lseek(run->dynamicFileFd, (off_t)0, SEEK_SET) == (off_t)-1) {
        PLOG_E("lseek(fd=%d, 0, SEEK_SET)", run->dynamicFileFd);
        return false;
    }
    struct stat st;
    if (fstat(run->dynamicFileFd, &st) == -1) {
        PLOG_E("fstat(fd=%d)", run->dynamicFileFd);
        return false;
    }
    if (st.st_size <= _HF_INPUT_MAX_SIZE) {
        run->dynamicFileSz = (size_t)st.st_size;
    } else {
        run->dynamicFileSz = _HF_INPUT_MAX_SIZE;
        LOG_W("External tool created too large of a file, '%zu', truncating it to '%zu'",
            (size_t)st.st_size, run->dynamicFileSz);
    }
    if (ftruncate(run->dynamicFileFd, _HF_INPUT_MAX_SIZE) == -1) {
        PLOG_E("ftruncate(fd=%d, size=%zu)", run->dynamicFileFd, (size_t)_HF_INPUT_MAX_SIZE);
        return false;
    }
    return true;
}

static bool fuzz_prepareFileDynamically(run_t* run) {
    run->origFileName = "[DYNAMIC]";

    {
        MX_SCOPED_RWLOCK_READ(&run->global->dynfileq_mutex);

        if (run->global->dynfileqCnt == 0) {
            LOG_F(
                "The dynamic file corpus is empty. Apparently, the initial fuzzing of the "
                "provided file corpus (-f) has not produced any follow-up files with positive "
                "coverage and/or CPU counters");
        }

        if (run->dynfileqCurrent == NULL) {
            run->dynfileqCurrent = TAILQ_FIRST(&run->global->dynfileq);
        } else {
            if (run->dynfileqCurrent == TAILQ_LAST(&run->global->dynfileq, dyns_t)) {
                run->dynfileqCurrent = TAILQ_FIRST(&run->global->dynfileq);
            } else {
                run->dynfileqCurrent = TAILQ_NEXT(run->dynfileqCurrent, pointers);
            }
        }
    }

    memcpy(run->dynamicFile, run->dynfileqCurrent->data, run->dynfileqCurrent->size);
    run->dynamicFileSz = run->dynfileqCurrent->size;

    mangle_mangleContent(run);

    return true;
}

static bool fuzz_prepareFile(run_t* run, bool rewind) {
    char fname[PATH_MAX];
    if (input_getNext(run, fname, rewind) == false) {
        return false;
    }
    run->origFileName = files_basename(fname);

    ssize_t fileSz = files_readFileToBufMax(fname, run->dynamicFile, run->global->maxFileSz);
    if (fileSz < 0) {
        LOG_E("Couldn't read contents of '%s'", fname);
        return false;
    }
    run->dynamicFileSz = fileSz;

    mangle_mangleContent(run);

    return true;
}

static bool fuzz_prepareFileExternally(run_t* run) {
    static __thread char ofname[PATH_MAX];
    if (input_getNext(run, ofname, /* rewind= */ true)) {
        run->origFileName = files_basename(ofname);
    } else {
        run->origFileName = "[EXTERNAL]";
    }

    char fname[PATH_MAX];
    snprintf(fname, sizeof(fname), "/dev/fd/%d", run->dynamicFileFd);

    const char* const argv[] = {run->global->exe.externalCommand, fname, NULL};
    if (subproc_System(run, argv) != 0) {
        LOG_E("Subprocess '%s' returned abnormally", run->global->exe.externalCommand);
        return false;
    }
    LOG_D("Subporcess '%s' finished with success", run->global->exe.externalCommand);

    if (!fuzz_checkSizeNRewind(run)) {
        return false;
    }

    return true;
}

static bool fuzz_postProcessFile(run_t* run) {
    char fname[PATH_MAX];
    snprintf(fname, sizeof(fname), "/dev/fd/%d", run->dynamicFileFd);

    const char* const argv[] = {run->global->exe.postExternalCommand, fname, NULL};
    if (subproc_System(run, argv) != 0) {
        LOG_E("Subprocess '%s' returned abnormally", run->global->exe.postExternalCommand);
        return false;
    }
    LOG_D("Subporcess '%s' finished with success", run->global->exe.externalCommand);

    if (!fuzz_checkSizeNRewind(run)) {
        return false;
    }

    return true;
}

static fuzzState_t fuzz_getState(run_t* run) { return ATOMIC_GET(run->global->state); }

static void fuzz_setDynamicMainState(run_t* run) {
    /* All threads need to indicate willingness to switch to the DYNAMIC_MAIN state */
    static uint32_t cnt = 0;
    ATOMIC_PRE_INC(cnt);

    static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
    MX_SCOPED_LOCK(&state_mutex);

    if (fuzz_getState(run) == _HF_STATE_DYNAMIC_MAIN) {
        return;
    }

    for (;;) {
        /* Check if all threads have reported in for changing state */
        if (ATOMIC_GET(cnt) == run->global->threads.threadsMax) {
            break;
        }
        if (fuzz_isTerminating()) {
            return;
        }
        usleep(1000 * 10); /* 10ms */
    }

    LOG_I("Entering phase 2/2: Main");
    ATOMIC_SET(run->global->state, _HF_STATE_DYNAMIC_MAIN);
}

static bool fuzz_writeCovFile(const char* dir, const uint8_t* data, size_t len) {
    char fname[PATH_MAX];

    uint64_t crc64f = util_CRC64(data, len);
    uint64_t crc64r = util_CRC64Rev(data, len);
    snprintf(fname, sizeof(fname), "%s/%016" PRIx64 "%016" PRIx64 ".%08" PRIx32 ".honggfuzz.cov",
        dir, crc64f, crc64r, (uint32_t)len);

    if (files_exists(fname)) {
        LOG_D("File '%s' already exists in the output corpus directory '%s'", fname, dir);
        return true;
    }

    LOG_D("Adding file '%s' to the corpus directory '%s'", fname, dir);

    if (!files_writeBufToFile(fname, data, len, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC)) {
        LOG_W("Couldn't write buffer to file '%s'", fname);
        return false;
    }

    return true;
}

static void fuzz_addFileToFileQ(run_t* run) {
    struct dynfile_t* dynfile = (struct dynfile_t*)util_Malloc(sizeof(struct dynfile_t));
    dynfile->size = run->dynamicFileSz;
    dynfile->data = (uint8_t*)util_Malloc(run->dynamicFileSz);
    memcpy(dynfile->data, run->dynamicFile, run->dynamicFileSz);

    MX_SCOPED_RWLOCK_WRITE(&run->global->dynfileq_mutex);
    TAILQ_INSERT_TAIL(&run->global->dynfileq, dynfile, pointers);
    run->global->dynfileqCnt++;

    if (!fuzz_writeCovFile(run->global->io.covDirAll, run->dynamicFile, run->dynamicFileSz)) {
        LOG_E("Couldn't save the coverage data to '%s'", run->global->io.covDirAll);
    }

    /* No need to add files to the new coverage dir, if this is just the dry-run phase */
    if (fuzz_getState(run) == _HF_STATE_DYNAMIC_PRE || run->global->io.covDirNew == NULL) {
        return;
    }

    if (!fuzz_writeCovFile(run->global->io.covDirNew, run->dynamicFile, run->dynamicFileSz)) {
        LOG_E("Couldn't save the new coverage data to '%s'", run->global->io.covDirNew);
    }
}

static void fuzz_perfFeedback(run_t* run) {
    if (run->global->skipFeedbackOnTimeout && run->tmOutSignaled) {
        return;
    }

    LOG_D("New file size: %zu, Perf feedback new/cur (instr,branch): %" PRIu64 "/%" PRIu64
          "/%" PRIu64 "/%" PRIu64 ", BBcnt new/total: %" PRIu64 "/%" PRIu64,
        run->dynamicFileSz, run->linux.hwCnts.cpuInstrCnt, run->global->linux.hwCnts.cpuInstrCnt,
        run->linux.hwCnts.cpuBranchCnt, run->global->linux.hwCnts.cpuBranchCnt,
        run->linux.hwCnts.newBBCnt, run->global->linux.hwCnts.bbCnt);

    MX_SCOPED_LOCK(&run->global->feedback_mutex);

    uint64_t softCntPc = 0;
    uint64_t softCntEdge = 0;
    uint64_t softCntCmp = 0;
    if (run->global->bbFd != -1) {
        softCntPc = ATOMIC_GET(run->global->feedback->pidFeedbackPc[run->fuzzNo]);
        ATOMIC_CLEAR(run->global->feedback->pidFeedbackPc[run->fuzzNo]);
        softCntEdge = ATOMIC_GET(run->global->feedback->pidFeedbackEdge[run->fuzzNo]);
        ATOMIC_CLEAR(run->global->feedback->pidFeedbackEdge[run->fuzzNo]);
        softCntCmp = ATOMIC_GET(run->global->feedback->pidFeedbackCmp[run->fuzzNo]);
        ATOMIC_CLEAR(run->global->feedback->pidFeedbackCmp[run->fuzzNo]);
    }

    int64_t diff0 = run->global->linux.hwCnts.cpuInstrCnt - run->linux.hwCnts.cpuInstrCnt;
    int64_t diff1 = run->global->linux.hwCnts.cpuBranchCnt - run->linux.hwCnts.cpuBranchCnt;

    /*
     * Coverage is the primary counter, the rest is secondary, and taken into consideration only
     * if the coverage counter has not been changed
     */
    if (run->linux.hwCnts.newBBCnt > 0 || softCntPc > 0 || softCntEdge > 0 || softCntCmp > 0 ||
        diff0 < 0 || diff1 < 0) {
        if (diff0 < 0) {
            run->global->linux.hwCnts.cpuInstrCnt = run->linux.hwCnts.cpuInstrCnt;
        }
        if (diff1 < 0) {
            run->global->linux.hwCnts.cpuBranchCnt = run->linux.hwCnts.cpuBranchCnt;
        }
        run->global->linux.hwCnts.bbCnt += run->linux.hwCnts.newBBCnt;
        run->global->linux.hwCnts.softCntPc += softCntPc;
        run->global->linux.hwCnts.softCntEdge += softCntEdge;
        run->global->linux.hwCnts.softCntCmp += softCntCmp;

        LOG_I("Size:%zu (i,b,hw,edge,ip,cmp): %" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
              "/%" PRIu64 "/%" PRIu64 ", Tot:%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
              "/%" PRIu64 "/%" PRIu64,
            run->dynamicFileSz, run->linux.hwCnts.cpuInstrCnt, run->linux.hwCnts.cpuBranchCnt,
            run->linux.hwCnts.newBBCnt, softCntEdge, softCntPc, softCntCmp,
            run->global->linux.hwCnts.cpuInstrCnt, run->global->linux.hwCnts.cpuBranchCnt,
            run->global->linux.hwCnts.bbCnt, run->global->linux.hwCnts.softCntEdge,
            run->global->linux.hwCnts.softCntPc, run->global->linux.hwCnts.softCntCmp);

        fuzz_addFileToFileQ(run);
    }
}

static void fuzz_sanCovFeedback(run_t* run) {
    if (run->global->skipFeedbackOnTimeout && run->tmOutSignaled) {
        return;
    }

    LOG_D("File size (Best/New): %zu, SanCov feedback (bb,dso): Best: [%" PRIu64 ",%" PRIu64
          "] / New: [%" PRIu64 ",%" PRIu64 "], newBBs:%" PRIu64,
        run->dynamicFileSz, run->global->sanCovCnts.hitBBCnt, run->global->sanCovCnts.iDsoCnt,
        run->sanCovCnts.hitBBCnt, run->sanCovCnts.iDsoCnt, run->sanCovCnts.newBBCnt);

    MX_SCOPED_LOCK(&run->global->feedback_mutex);

    int64_t diff0 = run->global->linux.hwCnts.cpuInstrCnt - run->linux.hwCnts.cpuInstrCnt;
    int64_t diff1 = run->global->linux.hwCnts.cpuBranchCnt - run->linux.hwCnts.cpuBranchCnt;

    /*
     * Keep mutated seed if:
     *  a) Newly discovered (not met before) BBs
     *  b) More instrumented DSOs loaded
     *
     * TODO: (a) method can significantly assist to further improvements in interesting areas
     * discovery if combined with seeds pool/queue support. If a runtime queue is maintained
     * more interesting seeds can be saved between runs instead of instantly discarded
     * based on current absolute elitism (only one mutated seed is promoted).
     */

    bool newCov =
        (run->sanCovCnts.newBBCnt > 0 || run->global->sanCovCnts.iDsoCnt < run->sanCovCnts.iDsoCnt);

    if (newCov || (diff0 < 0 || diff1 < 0)) {
        LOG_I("SanCov Update: fsize:%zu, newBBs:%" PRIu64 ", (Cur,New): %" PRIu64 "/%" PRIu64
              ",%" PRIu64 "/%" PRIu64,
            run->dynamicFileSz, run->sanCovCnts.newBBCnt, run->global->sanCovCnts.hitBBCnt,
            run->global->sanCovCnts.iDsoCnt, run->sanCovCnts.hitBBCnt, run->sanCovCnts.iDsoCnt);

        run->global->sanCovCnts.hitBBCnt += run->sanCovCnts.newBBCnt;
        run->global->sanCovCnts.dsoCnt = run->sanCovCnts.dsoCnt;
        run->global->sanCovCnts.iDsoCnt = run->sanCovCnts.iDsoCnt;
        run->global->sanCovCnts.crashesCnt += run->sanCovCnts.crashesCnt;
        run->global->sanCovCnts.newBBCnt = run->sanCovCnts.newBBCnt;

        if (run->global->sanCovCnts.totalBBCnt < run->sanCovCnts.totalBBCnt) {
            /* Keep only the max value (for dlopen cases) to measure total target coverage */
            run->global->sanCovCnts.totalBBCnt = run->sanCovCnts.totalBBCnt;
        }

        run->global->linux.hwCnts.cpuInstrCnt = run->linux.hwCnts.cpuInstrCnt;
        run->global->linux.hwCnts.cpuBranchCnt = run->linux.hwCnts.cpuBranchCnt;

        fuzz_addFileToFileQ(run);
    }
}

/* Return value indicates whether report file should be updated with the current verified crash */
static bool fuzz_runVerifier(run_t* run) {
    if (!run->crashFileName[0] || !run->backtrace) {
        return false;
    }

    uint64_t backtrace = run->backtrace;

    char origCrashPath[PATH_MAX];
    snprintf(origCrashPath, sizeof(origCrashPath), "%s", run->crashFileName);
    /* Workspace is inherited, just append a extra suffix */
    char verFile[PATH_MAX];
    snprintf(verFile, sizeof(verFile), "%s.verified", origCrashPath);

    if (files_exists(verFile)) {
        LOG_D("Crash file to verify '%s' is already verified as '%s'", origCrashPath, verFile);
        return false;
    }

    for (int i = 0; i < _HF_VERIFIER_ITER; i++) {
        LOG_I("Launching verifier for HASH: %" PRIx64 " (iteration: %d)", run->backtrace, i);
        run->timeStartedMillis = 0;
        run->backtrace = 0;
        run->access = 0;
        run->exception = 0;
        run->mainWorker = false;

        if (!subproc_Run(run)) {
            LOG_F("subproc_Run()");
        }

        /* If stack hash doesn't match skip name tag and exit */
        if (run->backtrace != backtrace) {
            LOG_E("Verifier stack mismatch: (original) %" PRIx64 " != (new) %" PRIx64, backtrace,
                run->backtrace);
            run->backtrace = backtrace;
            return true;
        }
    }

    /* Copy file with new suffix & remove original copy */
    int fd = TEMP_FAILURE_RETRY(open(verFile, O_CREAT | O_EXCL | O_WRONLY, 0600));
    if (fd == -1 && errno == EEXIST) {
        LOG_I("It seems that '%s' already exists, skipping", verFile);
        return false;
    }
    if (fd == -1) {
        PLOG_E("Couldn't create '%s'", verFile);
        return true;
    }
    defer { close(fd); };
    if (!files_writeToFd(fd, run->dynamicFile, run->dynamicFileSz)) {
        LOG_E("Couldn't save verified file as '%s'", verFile);
        unlink(verFile);
        return true;
    }

    LOG_I("Verified crash for HASH: %" PRIx64 " and saved it as '%s'", backtrace, verFile);
    ATOMIC_POST_INC(run->global->cnts.verifiedCrashesCnt);

    return true;
}

static void fuzz_fuzzLoop(run_t* run) {
    run->pid = 0;
    run->timeStartedMillis = 0;
    run->crashFileName[0] = '\0';
    run->pc = 0;
    run->backtrace = 0;
    run->access = 0;
    run->exception = 0;
    run->report[0] = '\0';
    run->mainWorker = true;
    run->origFileName = "DYNAMIC";
    run->mutationsPerRun = run->global->mutationsPerRun;
    run->dynamicFileSz = 0;

    run->sanCovCnts.hitBBCnt = 0;
    run->sanCovCnts.totalBBCnt = 0;
    run->sanCovCnts.dsoCnt = 0;
    run->sanCovCnts.newBBCnt = 0;
    run->sanCovCnts.crashesCnt = 0;

    run->linux.hwCnts.cpuInstrCnt = 0;
    run->linux.hwCnts.cpuBranchCnt = 0;
    run->linux.hwCnts.bbCnt = 0;
    run->linux.hwCnts.newBBCnt = 0;

    if (fuzz_getState(run) == _HF_STATE_DYNAMIC_PRE) {
        run->mutationsPerRun = 0U;
        if (fuzz_prepareFile(run, false /* rewind */) == false) {
            fuzz_setDynamicMainState(run);
        }
    }

    if (fuzz_isTerminating()) {
        return;
    }

    if (fuzz_getState(run) == _HF_STATE_DYNAMIC_MAIN) {
        if (run->global->exe.externalCommand) {
            if (!fuzz_prepareFileExternally(run)) {
                LOG_F("fuzz_prepareFileExternally() failed");
            }
        } else if (!fuzz_prepareFileDynamically(run)) {
            LOG_F("fuzz_prepareFileDynamically() failed");
        }

        if (run->global->exe.postExternalCommand) {
            if (!fuzz_postProcessFile(run)) {
                LOG_F("fuzz_postProcessFile() failed");
            }
        }
    }

    if (fuzz_getState(run) == _HF_STATE_STATIC) {
        if (run->global->exe.externalCommand) {
            if (!fuzz_prepareFileExternally(run)) {
                LOG_F("fuzz_prepareFileExternally() failed");
            }
        } else {
            if (!fuzz_prepareFile(run, true /* rewind */)) {
                LOG_F("fuzz_prepareFile() failed");
            }
        }

        if (run->global->exe.postExternalCommand != NULL) {
            if (!fuzz_postProcessFile(run)) {
                LOG_F("fuzz_postProcessFile() failed");
            }
        }
    }
    /* Truncate input file to the desired size */
    if (ftruncate(run->dynamicFileFd, run->dynamicFileSz) == -1) {
        PLOG_F("ftruncate(fd=%d, size=%zu)", run->dynamicFileFd, run->dynamicFileSz);
    }

    if (subproc_Run(run) == false) {
        LOG_F("subproc_Run()");
    }

    if (run->global->dynFileMethod != _HF_DYNFILE_NONE) {
        fuzz_perfFeedback(run);
    }
    if (run->global->useSanCov) {
        fuzz_sanCovFeedback(run);
    }
    if (run->global->useVerifier && !fuzz_runVerifier(run)) {
        return;
    }
    report_Report(run);
}

static void* fuzz_threadNew(void* arg) {
    honggfuzz_t* hfuzz = (honggfuzz_t*)arg;
    unsigned int fuzzNo = ATOMIC_POST_INC(hfuzz->threads.threadsActiveCnt);
    LOG_I("Launched new fuzzing thread, no. #%" PRId32, fuzzNo);

    run_t run = {
        .global = hfuzz,
        .pid = 0,
        .persistentPid = 0,
        .dynfileqCurrent = NULL,
        .dynamicFile = NULL,
        .dynamicFileFd = -1,
        .fuzzNo = fuzzNo,
        .persistentSock = -1,
        .tmOutSignaled = false,

        .linux.attachedPid = 0,
    };
    if ((run.dynamicFile = files_mapSharedMem(
             hfuzz->maxFileSz, &run.dynamicFileFd, run.global->io.workDir)) == MAP_FAILED) {
        LOG_F("Couldn't create an input file of size: %zu", hfuzz->maxFileSz);
    }
    defer { close(run.dynamicFileFd); };

    if (arch_archThreadInit(&run) == false) {
        LOG_F("Could not initialize the thread");
    }

    for (;;) {
        /* Reset and rewind the input file to the original maximum size */
        if (ftruncate(run.dynamicFileFd, hfuzz->maxFileSz) == -1) {
            PLOG_F("ftruncate(fd=%d, size=%zu)", run.dynamicFileFd, hfuzz->maxFileSz);
        }
        if (lseek(run.dynamicFileFd, (off_t)0, SEEK_SET) == (off_t)-1) {
            PLOG_F("lseek(fd=%d, 0, SEEK_SET)", run.dynamicFileFd);
        }

        /* Check if dry run mode with verifier enabled */
        if (run.global->mutationsPerRun == 0U && run.global->useVerifier) {
            if (ATOMIC_POST_INC(run.global->cnts.mutationsCnt) >= run.global->io.fileCnt) {
                ATOMIC_POST_INC(run.global->threads.threadsFinished);
                break;
            }
        }
        /* Check for max iterations limit if set */
        else if ((ATOMIC_POST_INC(run.global->cnts.mutationsCnt) >= run.global->mutationsMax) &&
                 run.global->mutationsMax) {
            ATOMIC_POST_INC(run.global->threads.threadsFinished);
            break;
        }

        fuzz_fuzzLoop(&run);

        if (fuzz_isTerminating()) {
            break;
        }

        if (run.global->exitUponCrash && ATOMIC_GET(run.global->cnts.crashesCnt) > 0) {
            LOG_I("Seen a crash. Terminating all fuzzing threads");
            fuzz_setTerminating();
            break;
        }
    }

    LOG_I("Terminating thread no. #%" PRId32, fuzzNo);
    ATOMIC_POST_INC(run.global->threads.threadsFinished);
    pthread_kill(run.global->threads.mainThread, SIGALRM);
    return NULL;
}

static void fuzz_runThread(honggfuzz_t* hfuzz, pthread_t* thread, void* (*thread_func)(void*)) {
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, _HF_PTHREAD_STACKSIZE);
    pthread_attr_setguardsize(&attr, (size_t)sysconf(_SC_PAGESIZE));

    if (pthread_create(thread, &attr, thread_func, (void*)hfuzz) < 0) {
        PLOG_F("Couldn't create a new thread");
    }

    pthread_attr_destroy(&attr);

    return;
}

void fuzz_threadsStart(honggfuzz_t* hfuzz, pthread_t* threads) {
    if (!arch_archInit(hfuzz)) {
        LOG_F("Couldn't prepare arch for fuzzing");
    }
    if (!sanitizers_Init(hfuzz)) {
        LOG_F("Couldn't prepare sanitizer options");
    }
    if (!sancov_Init(hfuzz)) {
        LOG_F("Couldn't prepare sancov options");
    }

    if (hfuzz->useSanCov || hfuzz->dynFileMethod != _HF_DYNFILE_NONE) {
        LOG_I("Entering phase 1/2: Dry Run");
        hfuzz->state = _HF_STATE_DYNAMIC_PRE;
    } else {
        LOG_I("Entering phase: Static");
        hfuzz->state = _HF_STATE_STATIC;
    }

    for (size_t i = 0; i < hfuzz->threads.threadsMax; i++) {
        fuzz_runThread(hfuzz, &threads[i], fuzz_threadNew);
    }
}

void fuzz_threadsStop(honggfuzz_t* hfuzz, pthread_t* threads) {
    for (size_t i = 0; i < hfuzz->threads.threadsMax; i++) {
        void* retval;
        if (pthread_join(threads[i], &retval) != 0) {
            PLOG_F("Couldn't pthread_join() thread: %zu", i);
        }
    }
    LOG_I("All threads done");
}
