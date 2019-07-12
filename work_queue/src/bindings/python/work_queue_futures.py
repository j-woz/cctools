## @package work_queue_futures
# Python Work Queue bindings.
#
# This is a library on top of work_queue which replaces q.wait with the concept
# of futures.
#
# This is experimental.
#
# - @ref work_queue_futures::WorkQueue
# - @ref work_queue::Task

import work_queue
import threading
import time
import traceback
import sys

try:
    # from py3
    import queue as ThreadQueue
except ImportError:
    # from py2
    import Queue as ThreadQueue

##
# Python Work Queue object
#
# Implements an asynchronous WorkQueueFutures object.
# @ref work_queue_futures::WorkQueueFutures.
class WorkQueueFutures(object):
    def __init__(self, *args, **kwargs):
        self._queue = work_queue.WorkQueue(*args, **kwargs)
        self._thread = threading.Thread(target = self._sync_loop)

        self._stop_queue_event = threading.Event()

        self._tasks_to_submit = ThreadQueue.Queue()

        # calls to synchronous WorkQueueFutures are coordinated with _queue_lock
        self._queue_lock     = threading.Lock()

        self._thread.daemon  = True
        self._thread.start()

    # methods not explicitly defined we route to synchronous WorkQueue, using a lock.
    def __getattr__(self, name):
        attr = getattr(self._queue, name)

        if callable(attr):
            def method_wrapped(*args, **kwargs):
                result = None
                with self._queue_lock:
                    result = attr(*args, **kwargs)
                return result
            return method_wrapped
        else:
            return attr

    ##
    # Submit a task to the queue.
    #
    # @param self   Reference to the current work queue object.
    # @param task   A task description created from @ref work_queue::Task.
    def submit(self, future_task):
        if isinstance(future_task, FutureTask):
            self._tasks_to_submit.put(future_task, False)
            return future_task
        else:
            raise TypeError("{} is not a WorkQueue.Task")

    ##
    # Disable wait when using the futures interface
    def wait(self, *args, **kwargs):
        raise AttributeError('wait cannot be used with the futures interface.')

    ##
    # Determine whether there are any known tasks queued, running, or waiting to be collected.
    #
    # Returns 0 if there are tasks remaining in the system, 1 if the system is "empty".
    #
    # @param self       Reference to the current work queue object.
    def empty(self):
        if self._tasks_to_submit.empty():
            return self._queue.empty()
        else:
            return 0

    def _sync_loop(self):
        # map from taskids to FutureTask objects
        active_tasks = {}

        while True:
            try:
                if self._stop_queue_event.is_set():
                    return

                # do the submits, if any
                while not self._tasks_to_submit.empty():
                    try:
                        future_task = self._tasks_to_submit.get(False)
                        if not future_task.cancelled():
                            with self._queue_lock:
                                taskid = self._queue.submit(future_task)
                                future_task._set_queue(self)
                                active_tasks[future_task.id] = future_task
                        self._tasks_to_submit.task_done()
                    except ThreadQueue.Empty:
                        pass

                # wait for any task
                with self._queue_lock:
                    if not self._queue.empty():
                        wq_task = self._queue.wait(1)
                        if wq_task:
                            wq_task.set_result_or_exception()
                            del active_tasks[wq_task.id]

            except Exception as e:
                # on error, we set exception to all the known tasks so that .result() does not block
                err = traceback.format_exc()
                while not self._tasks_to_submit.empty():
                    try:
                        t = self._tasks_to_submit.get(False)
                        t.set_exception(FutureTaskError(t, err))
                        self._tasks_to_submit.task_done()
                    except ThreadQueue.Empty:
                        pass
                for t in active_tasks.values():
                    t.set_exception(FutureTaskError(t, err))
                active_tasks.clear()
                self._stop_queue_event.set()

    def join(self):
        while not self.empty() and not self._stop_queue_event.is_set():
            time.sleep(1)

    def _terminate(self):
        self._stop_queue_event.set()
        self._thread.join()

    def __del__(self):
        self._terminate()

import threading
import concurrent.futures as futures

class FutureTask(work_queue.Task):
    def __init__(self, command):
        super(FutureTask, self).__init__(command)

        self._queue     = None
        self._cancelled = False
        self._exception = None

        self._done_event = threading.Event()
        self._callbacks = []

    @property
    def queue(self):
        return self._queue

    def _set_queue(self, queue):
        self._queue = queue
        self.set_running_or_notify_cancel()

    def cancel(self):
        if self.queue:
            self.queue.cancel_by_taskid(self.id)

        self._cancelled = True
        self._done_event.set()
        self._invoke_callbacks()

        return self.cancelled()

    def cancelled(self):
        return self._cancelled

    def done(self):
        return self._done_event.is_set()

    def running(self):
        return (self._queue is not None) and (not self.done())

    def result(self, timeout=None):
        if self.cancelled():
            raise CancelledError

        # wait for task to be done event if not done already
        self._done_event.wait(timeout)

        if self.done():
            if self._exception is not None:
                raise self._exception
            else:
                return self._result
        else:
            # Raise exception if task not done by timeout
            raise futures.TimeoutError(timeout)

    def exception(self, timeout=None):
        if self.cancelled():
            raise CancelledError

        self._done_event.wait(timeout)

        if self.done():
            return self._exception
        else:
            raise futures.TimeoutError(timeout)


    def add_done_callback(self, fn):
        """
        Attaches the callable fn to the future. fn will be called, with the
        future as its only argument, when the future is cancelled or finishes
        running.  Added callables are called in the order that they were added
        and are always called in a thread belonging to the process that added
        them.

        If the callable raises an Exception subclass, it will be logged and
        ignored. If the callable raises a BaseException subclass, the behavior
        is undefined.

        If the future has already completed or been cancelled, fn will be
        called immediately.
        """

        if self.done():
            fn(self)
        else:
            self._callbacks.append(fn)

    def _invoke_callbacks(self):
        for fn in self._callbacks:
            try:
                fn(self)
            except Exception as e:
                sys.stderr.write('Error when executing future object callback:\n')
                traceback.print_exc()


    def set_result_or_exception(self):
        result = self._task.result
        if result == work_queue.WORK_QUEUE_RESULT_SUCCESS and self.return_status == 0:
            self.set_result(self.output)
        else:
            self.set_exception(FutureTaskError(self))

    def set_running_or_notify_cancel(self):
        if self.cancelled():
            return False
        else:
            return True

    def set_result(self, result):
        self._result = result
        self._done_event.set()
        self._invoke_callbacks()

    def set_exception(self, exception):
        self._exception = exception
        self._done_event.set()
        self._invoke_callbacks()


class FutureTaskError(Exception):
    _state_to_msg = {
        work_queue.WORK_QUEUE_RESULT_SUCCESS:             'Success',
        work_queue.WORK_QUEUE_RESULT_INPUT_MISSING:       'Input file is missing',
        work_queue.WORK_QUEUE_RESULT_OUTPUT_MISSING:      'Output file is missing',
        work_queue.WORK_QUEUE_RESULT_STDOUT_MISSING:      'stdout is missing',
        work_queue.WORK_QUEUE_RESULT_SIGNAL:              'Signal received',
        work_queue.WORK_QUEUE_RESULT_RESOURCE_EXHAUSTION: 'Resources exhausted',
        work_queue.WORK_QUEUE_RESULT_TASK_TIMEOUT:        'Task timed-out before completion',
        work_queue.WORK_QUEUE_RESULT_UNKNOWN:             'Unknown error',
        work_queue.WORK_QUEUE_RESULT_FORSAKEN:            'Internal error',
        work_queue.WORK_QUEUE_RESULT_MAX_RETRIES:         'Maximum number of retries reached',
        work_queue.WORK_QUEUE_RESULT_TASK_MAX_RUN_TIME:   'Task did not finish before deadline',
        work_queue.WORK_QUEUE_RESULT_DISK_ALLOC_FULL:     'Disk allocation for the task is full'
    }

    def __init__(self, task, exception = None):
        self.task  = task

        self.exit_status = None
        self.state       = None
        self.exception   = None

        if exception:
            self.exception = exception
        else:
            self.exit_status = task.return_status
            self.state       = task._task.result

    def __str__(self):
        if self.exception:
            return str(self.exception)

        msg = self._state_to_str()
        if not msg:
            return str(self.state)

        if self.state != work_queue.WORK_QUEUE_RESULT_SUCCESS or self.exit_status == 0:
            return msg
        else:
            return 'Execution completed with exit status {}'.format(self.exit_status)

    def _state_to_str(self):
        return FutureTaskError._state_to_msg.get(self.state, None)

