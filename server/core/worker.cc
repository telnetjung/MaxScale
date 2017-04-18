/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "maxscale/worker.hh"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <maxscale/alloc.h>
#include <maxscale/atomic.h>
#include <maxscale/config.h>
#include <maxscale/hk_heartbeat.h>
#include <maxscale/log_manager.h>
#include <maxscale/platform.h>
#include "maxscale/modules.h"
#include "maxscale/poll.h"

#define WORKER_ABSENT_ID -1

using maxscale::Worker;

// TODO: Temporarily moved here.
POLL_STATS *pollStats = NULL;
// TODO: Temporarily moved here.
QUEUE_STATS* queueStats = NULL;

namespace
{

/**
 * Unit variables.
 */
static struct this_unit
{
    int      n_workers;         // How many workers there are.
    Worker** ppWorkers;         // Array of worker instances.
    int      number_poll_spins; // Maximum non-block polls
    int      max_poll_sleep;    // Maximum block time

} this_unit =
{
    0,
    NULL
};

static thread_local struct this_thread
{
    int current_worker_id; // The worker id of the current thread
} this_thread =
{
    WORKER_ABSENT_ID
};

/**
 * Structure used for sending cross-thread messages.
 */
typedef struct worker_message
{
    uint32_t id;   /*< Message id. */
    intptr_t arg1; /*< Message specific first argument. */
    intptr_t arg2; /*< Message specific second argument. */
} WORKER_MESSAGE;

/**
 * Check error returns from epoll_ctl; impossible ones lead to crash.
 *
 * @param wid        Worker id.
 * @param errornum   The errno set by epoll_ctl
 * @param op         Either EPOLL_CTL_ADD or EPOLL_CTL_DEL.
 */
void poll_resolve_error(int wid, int fd, int errornum, int op)
{
    if (op == EPOLL_CTL_ADD)
    {
        if (EEXIST == errornum)
        {
            MXS_ERROR("File descriptor %d already added to epoll instance of worker %d.", fd, wid);
            return;
        }

        if (ENOSPC == errornum)
        {
            MXS_ERROR("The limit imposed by /proc/sys/fs/epoll/max_user_watches was "
                      "reached when trying to add file descriptor %d to epoll instance "
                      "of worker %d.", fd, wid);
            return;
        }
    }
    else
    {
        ss_dassert(op == EPOLL_CTL_DEL);

        /* Must be removing */
        if (ENOENT == errornum)
        {
            MXS_ERROR("File descriptor %d was not found in epoll instance of worker %d.", fd, wid);
            return;
        }
    }

    /* Common checks for add or remove - crash MaxScale */
    if (EBADF == errornum)
    {
        raise(SIGABRT);
    }
    if (EINVAL == errornum)
    {
        raise(SIGABRT);
    }
    if (ENOMEM == errornum)
    {
        raise(SIGABRT);
    }
    if (EPERM == errornum)
    {
        raise(SIGABRT);
    }

    /* Undocumented error number */
    raise(SIGABRT);
}

}

static bool modules_thread_init();
static void modules_thread_finish();

Worker::Worker(int id,
               int epoll_fd,
               POLL_STATS* pPoll_stats,
               QUEUE_STATS* pQueue_stats)
    : m_id(id)
    , m_epoll_fd(epoll_fd)
    , m_pPoll_stats(pPoll_stats)
    , m_pQueue_stats(pQueue_stats)
    , m_pQueue(NULL)
    , m_thread(0)
    , m_started(false)
    , m_should_shutdown(false)
    , m_shutdown_initiated(false)
{
}

Worker::~Worker()
{
    ss_dassert(!m_started);

    delete m_pQueue;
    close(m_epoll_fd);
}

// static
void Worker::init()
{
    this_unit.n_workers = config_threadcount();
    this_unit.number_poll_spins = config_nbpolls();
    this_unit.max_poll_sleep = config_pollsleep();

    pollStats = (POLL_STATS*)MXS_CALLOC(this_unit.n_workers, sizeof(POLL_STATS));
    if (!pollStats)
    {
        exit(-1);
    }

    for (int i = 0; i < this_unit.n_workers; i++)
    {
        pollStats[i].thread_state = THREAD_STOPPED;
    }

    queueStats = (QUEUE_STATS*)MXS_CALLOC(this_unit.n_workers, sizeof(QUEUE_STATS));
    if (!queueStats)
    {
        exit(-1);
    }

    this_unit.ppWorkers = new (std::nothrow) Worker* [this_unit.n_workers] (); // Zero initialized array

    if (!this_unit.ppWorkers)
    {
        // If we cannot allocate the array, we just exit.
        exit(-1);
    }

    for (int i = 0; i < this_unit.n_workers; ++i)
    {
        Worker* pWorker = Worker::create(i, &pollStats[i], &queueStats[i]);

        if (pWorker)
        {
            this_unit.ppWorkers[i] = pWorker;
        }
        else
        {
            // If a worker cannot be created, we just exit. No way we can continue.
            exit(-1);
        }
    }

    MXS_NOTICE("Workers created!");
}

void Worker::finish()
{
    for (int i = 0; i < this_unit.n_workers; ++i)
    {
        Worker* pWorker = this_unit.ppWorkers[i];

        delete pWorker;
        this_unit.ppWorkers[i] = NULL;
    }
}

bool Worker::add_fd(int fd, uint32_t events, MXS_POLL_DATA* pData)
{
    bool rv = true;

    events |= EPOLLET;

    struct epoll_event ev;

    ev.events = events;
    ev.data.ptr = pData;

    pData->thread.id = m_id;

    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0)
    {
        poll_resolve_error(m_id, fd, errno, EPOLL_CTL_ADD);
        rv = false;
    }

    return rv;
}

bool Worker::remove_fd(int fd)
{
    bool rv = true;

    struct epoll_event ev = {};

    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, &ev) != 0)
    {
        poll_resolve_error(m_id, fd, errno, EPOLL_CTL_DEL);
        rv = false;
    }

    return rv;
}

int mxs_worker_id(MXS_WORKER* pWorker)
{
    return static_cast<Worker*>(pWorker)->id();
}

bool mxs_worker_should_shutdown(MXS_WORKER* pWorker)
{
    return static_cast<Worker*>(pWorker)->should_shutdown();
}

Worker* Worker::get(int worker_id)
{
    ss_dassert(worker_id < this_unit.n_workers);

    return this_unit.ppWorkers[worker_id];
}

MXS_WORKER* mxs_worker_get(int worker_id)
{
    return Worker::get(worker_id);
}

Worker* Worker::get_current()
{
    Worker* pWorker = NULL;

    int worker_id = get_current_id();

    if (worker_id != WORKER_ABSENT_ID)
    {
        pWorker = Worker::get(worker_id);
    }

    return pWorker;
}

int Worker::get_current_id()
{
    return this_thread.current_worker_id;
}

//static
void Worker::set_nonblocking_polls(unsigned int nbpolls)
{
    this_unit.number_poll_spins = nbpolls;
}

//static
void Worker::set_maxwait(unsigned int maxwait)
{
    this_unit.max_poll_sleep = maxwait;
}

bool Worker::post_message(uint32_t msg_id, intptr_t arg1, intptr_t arg2)
{
    // NOTE: No logging here, this function must be signal safe.
    MessageQueue::Message message(msg_id, arg1, arg2);

    return m_pQueue->post(message);
}

bool mxs_worker_post_message(MXS_WORKER* pWorker, uint32_t msg_id, intptr_t arg1, intptr_t arg2)
{
    return static_cast<Worker*>(pWorker)->post_message(msg_id, arg1, arg2);
}

size_t Worker::broadcast_message(uint32_t msg_id, intptr_t arg1, intptr_t arg2)
{
    // NOTE: No logging here, this function must be signal safe.

    size_t n = 0;

    for (int i = 0; i < this_unit.n_workers; ++i)
    {
        Worker* pWorker = this_unit.ppWorkers[i];

        if (pWorker->post_message(msg_id, arg1, arg2))
        {
            ++n;
        }
    }

    return n;
}

size_t mxs_worker_broadcast_message(uint32_t msg_id, intptr_t arg1, intptr_t arg2)
{
    return Worker::broadcast_message(msg_id, arg1, arg2);
}

namespace
{

bool should_shutdown(void* pData)
{
    return static_cast<Worker*>(pData)->should_shutdown();
}

}

void Worker::run()
{
    this_thread.current_worker_id = m_id;
    poll_waitevents(m_epoll_fd, m_id,
                    m_pPoll_stats, m_pQueue_stats,
                    ::should_shutdown, this);
    this_thread.current_worker_id = WORKER_ABSENT_ID;

    MXS_NOTICE("Worker %d has shut down.", m_id);
}

bool Worker::start()
{
    m_started = true;

    if (!thread_start(&m_thread, &Worker::thread_main, this))
    {
        m_started = false;
    }

    return m_started;
}

void Worker::join()
{
    if (m_started)
    {
        MXS_NOTICE("Waiting for worker %d.", m_id);
        thread_wait(m_thread);
        MXS_NOTICE("Waited for worker %d.", m_id);
        m_started = false;
    }
}

void Worker::shutdown()
{
    // NOTE: No logging here, this function must be signal safe.

    if (!m_shutdown_initiated)
    {
        if (post_message(MXS_WORKER_MSG_SHUTDOWN, 0, 0))
        {
            m_shutdown_initiated = true;
        }
    }
}

void Worker::shutdown_all()
{
    // NOTE: No logging here, this function must be signal safe.

    for (int i = 0; i < this_unit.n_workers; ++i)
    {
        Worker* pWorker = this_unit.ppWorkers[i];

        pWorker->shutdown();
    }
}

/**
 * Creates a worker instance.
 * - Allocates the structure.
 * - Creates a pipe.
 * - Adds the read descriptor to the polling mechanism.
 *
 * @param worker_id     The id of the worker.
 * @param pPoll_stats   The poll statistics of the worker.
 * @param pQueue_stats  The queue statistics of the worker.
 *
 * @return A worker instance if successful, otherwise NULL.
 */
//static
Worker* Worker::create(int worker_id,
                       POLL_STATS* pPoll_stats,
                       QUEUE_STATS* pQueue_stats)
{
    Worker* pThis = NULL;

    int epoll_fd = epoll_create(MAX_EVENTS);

    if (epoll_fd != -1)
    {
        pThis = new (std::nothrow) Worker(worker_id, epoll_fd, pPoll_stats, pQueue_stats);

        if (pThis)
        {
            MessageQueue* pQueue = MessageQueue::create(pThis);

            if (pQueue)
            {
                if (pQueue->add_to_worker(pThis))
                {
                    pThis->m_pQueue = pQueue;
                }
                else
                {
                    MXS_ERROR("Could not add message queue to worker.");
                    delete pThis;
                    pThis = NULL;
                }
            }
            else
            {
                MXS_ERROR("Could not create message queue for worker.");
                delete pThis;
            }
        }
        else
        {
            MXS_OOM();
            close(epoll_fd);
        }
    }
    else
    {
        MXS_ERROR("Could not create epoll-instance for worker: %s", mxs_strerror(errno));
    }

    return pThis;
}

/**
 * The worker message handler.
 *
 * @param msg_id  The message id.
 * @param arg1    Message specific first argument.
 * @param arg2    Message specific second argument.
 */
void Worker::handle_message(MessageQueue& queue, const MessageQueue::Message& msg)
{
    switch  (msg.id())
    {
    case MXS_WORKER_MSG_PING:
        {
            ss_dassert(msg.arg1() == 0);
            char* zArg2 = reinterpret_cast<char*>(msg.arg2());
            const char* zMessage = zArg2 ? zArg2 : "Alive and kicking";
            MXS_NOTICE("Worker[%d]: %s.", m_id, zMessage);
            MXS_FREE(zArg2);
        }
        break;

    case MXS_WORKER_MSG_SHUTDOWN:
        {
            MXS_NOTICE("Worker %d received shutdown message.", m_id);
            m_should_shutdown = true;
        }
        break;

    case MXS_WORKER_MSG_CALL:
        {
            void (*f)(int, void*) = (void (*)(int,void*))msg.arg1();

            f(m_id, (void*)msg.arg2());
        }
        break;

    default:
        MXS_ERROR("Worker received unknown message %d.", msg.id());
    }
}

/**
 * The entry point of each worker thread.
 *
 * @param arg A worker.
 */
//static
void Worker::thread_main(void* pArg)
{
    if (modules_thread_init())
    {
        Worker* pWorker = static_cast<Worker*>(pArg);

        pWorker->run();

        modules_thread_finish();
    }
    else
    {
        MXS_ERROR("Could not perform thread initialization for all modules. Thread exits.");
    }
}

/**
 * The main polling loop
 *
 * @param epoll_fd         The epoll descriptor.
 * @param thread_id        The id of the calling thread.
 * @param poll_stats       The polling stats of the calling thread.
 * @param queue_stats      The queue stats of the calling thread.
 * @param should_shutdown  Pointer to function returning true if the polling should
 *                         be terminated.
 * @param data             Data provided to the @c should_shutdown function.
 */
//static
void Worker::poll_waitevents(int epoll_fd,
                             int thread_id,
                             POLL_STATS* poll_stats,
                             QUEUE_STATS* queue_stats,
                             bool (*should_shutdown)(void* data),
                             void* data)
{
    struct epoll_event events[MAX_EVENTS];
    int i, nfds, timeout_bias = 1;
    int poll_spins = 0;

    poll_stats->thread_state = THREAD_IDLE;

    while (!should_shutdown(data))
    {
        poll_stats->thread_state = THREAD_POLLING;

        atomic_add_int64(&poll_stats->n_polls, 1);
        if ((nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 0)) == -1)
        {
            int eno = errno;
            errno = 0;
            MXS_DEBUG("%lu [poll_waitevents] epoll_wait returned "
                      "%d, errno %d",
                      pthread_self(),
                      nfds,
                      eno);
        }
        /*
         * If there are no new descriptors from the non-blocking call
         * and nothing to process on the event queue then for do a
         * blocking call to epoll_wait.
         *
         * We calculate a timeout bias to alter the length of the blocking
         * call based on the time since we last received an event to process
         */
        else if (nfds == 0 && poll_spins++ > this_unit.number_poll_spins)
        {
            if (timeout_bias < 10)
            {
                timeout_bias++;
            }
            atomic_add_int64(&poll_stats->blockingpolls, 1);
            nfds = epoll_wait(epoll_fd,
                              events,
                              MAX_EVENTS,
                              (this_unit.max_poll_sleep * timeout_bias) / 10);
            if (nfds == 0)
            {
                poll_spins = 0;
            }
        }

        if (nfds > 0)
        {
            poll_stats->evq_length = nfds;
            if (nfds > poll_stats->evq_max)
            {
                poll_stats->evq_max = nfds;
            }

            timeout_bias = 1;
            if (poll_spins <= this_unit.number_poll_spins + 1)
            {
                atomic_add_int64(&poll_stats->n_nbpollev, 1);
            }
            poll_spins = 0;
            MXS_DEBUG("%lu [poll_waitevents] epoll_wait found %d fds",
                      pthread_self(),
                      nfds);
            atomic_add_int64(&poll_stats->n_pollev, 1);

            poll_stats->thread_state = THREAD_PROCESSING;

            poll_stats->n_fds[(nfds < MAXNFDS ? (nfds - 1) : MAXNFDS - 1)]++;
        }

        uint64_t cycle_start = hkheartbeat;

        for (int i = 0; i < nfds; i++)
        {
            /** Calculate event queue statistics */
            int64_t started = hkheartbeat;
            int64_t qtime = started - cycle_start;

            if (qtime > N_QUEUE_TIMES)
            {
                queue_stats->qtimes[N_QUEUE_TIMES]++;
            }
            else
            {
                queue_stats->qtimes[qtime]++;
            }

            queue_stats->maxqtime = MXS_MAX(queue_stats->maxqtime, qtime);

            MXS_POLL_DATA *data = (MXS_POLL_DATA*)events[i].data.ptr;

            uint32_t actions = data->handler(data, thread_id, events[i].events);

            if (actions & MXS_POLL_ACCEPT)
            {
                atomic_add_int64(&poll_stats->n_accept, 1);
            }

            if (actions & MXS_POLL_READ)
            {
                atomic_add_int64(&poll_stats->n_read, 1);
            }

            if (actions & MXS_POLL_WRITE)
            {
                atomic_add_int64(&poll_stats->n_write, 1);
            }

            if (actions & MXS_POLL_HUP)
            {
                atomic_add_int64(&poll_stats->n_hup, 1);
            }

            if (actions & MXS_POLL_ERROR)
            {
                atomic_add_int64(&poll_stats->n_error, 1);
            }

            /** Calculate event execution statistics */
            qtime = hkheartbeat - started;

            if (qtime > N_QUEUE_TIMES)
            {
                queue_stats->exectimes[N_QUEUE_TIMES]++;
            }
            else
            {
                queue_stats->exectimes[qtime % N_QUEUE_TIMES]++;
            }

            queue_stats->maxexectime = MXS_MAX(queue_stats->maxexectime, qtime);
        }

        dcb_process_idle_sessions(thread_id);

        poll_stats->thread_state = THREAD_ZPROCESSING;

        /** Process closed DCBs */
        dcb_process_zombies(thread_id);

        poll_check_message();

        poll_stats->thread_state = THREAD_IDLE;
    } /*< while(1) */

    poll_stats->thread_state = THREAD_STOPPED;
        }
/**
 * Calls thread_init on all loaded modules.
 *
 * @return True, if all modules were successfully initialized.
 */
static bool modules_thread_init()
{
    bool initialized = false;

    MXS_MODULE_ITERATOR i = mxs_module_iterator_get(NULL);
    MXS_MODULE* module = NULL;

    while ((module = mxs_module_iterator_get_next(&i)) != NULL)
    {
        if (module->thread_init)
        {
            int rc = (module->thread_init)();

            if (rc != 0)
            {
                break;
            }
        }
    }

    if (module)
    {
        // If module is non-NULL it means that the initialization failed for
        // that module. We now need to call finish on all modules that were
        // successfully initialized.
        MXS_MODULE* failed_module = module;
        i = mxs_module_iterator_get(NULL);

        while ((module = mxs_module_iterator_get_next(&i)) != failed_module)
        {
            if (module->thread_finish)
            {
                (module->thread_finish)();
            }
        }
    }
    else
    {
        initialized = true;
    }

    return initialized;
}

/**
 * Calls thread_finish on all loaded modules.
 */
static void modules_thread_finish()
{
    MXS_MODULE_ITERATOR i = mxs_module_iterator_get(NULL);
    MXS_MODULE* module = NULL;

    while ((module = mxs_module_iterator_get_next(&i)) != NULL)
    {
        if (module->thread_finish)
        {
            (module->thread_finish)();
        }
    }
}