# distutils: language = c++

import collections
import weakref

from fastrlock cimport rlock

# TODO(LWisteria): Use clEnqueueMapBuffer
from libc.stdlib cimport free
from libc.stdlib cimport malloc
# from clpy.backend import runtime
# from clpy.backend cimport runtime


class PinnedMemory(object):

    """Pinned memory allocation on host.

    This class provides a RAII interface of the pinned memory allocation.

    Args:
        size (int): Size of the memory allocation in bytes.

    """

    # NOTE(nsakabe-fixstars):
    # There isn't an equivalent of cudaHostAlloc in OpenCL.
    # (cudaHostAlloc'ed memory region can be used to
    #  store data for every device.
    #  In OpenCL, although there is CL_MEM_ALLOC_HOST_PTR,
    #  an allocated region is associated to a specific device.)
    # So it is difficult to implement PinnedMemory
    # with the same RAII interface.

    def __init__(self, Py_ssize_t size, unsigned int flags=0):
        self.size = size
        self.ptr = 0
        if size > 0:
            self.ptr = <size_t>malloc(size)
            # self.ptr = runtime.hostAlloc(size, flags)

    def __del__(self):
        if self.ptr:
            free(<void*>self.ptr)
            # runtime.freeHost(self.ptr)

    def __int__(self):
        """Returns the pointer value to the head of the allocation."""
        return self.ptr


cdef class PinnedMemoryPointer:

    """Pointer of a pinned memory.

    An instance of this class holds a reference to the original memory buffer
    and a pointer to a place within this buffer.

    Args:
        mem (PinnedMemory): The device memory buffer.
        offset (int): An offset from the head of the buffer to the place this
            pointer refers.

    Attributes:
        mem (PinnedMemory): The device memory buffer.
        ptr (int): Pointer to the place within the buffer.
    """

    def __init__(self, mem, Py_ssize_t offset):
        self.mem = mem
        self.ptr = mem.ptr + offset

    def __int__(self):
        """Returns the pointer value."""
        return self.ptr

    def __add__(x, y):
        """Adds an offset to the pointer."""
        cdef PinnedMemoryPointer self
        cdef Py_ssize_t offset
        if isinstance(x, PinnedMemoryPointer):
            self = x
            offset = <Py_ssize_t?>y
        else:
            self = <PinnedMemoryPointer?>y
            offset = <Py_ssize_t?>x
        return PinnedMemoryPointer(
            self.mem, self.ptr - self.mem.ptr + offset)

    def __iadd__(self, Py_ssize_t offset):
        """Adds an offset to the pointer in place."""
        self.ptr += offset
        return self

    def __sub__(self, offset):
        """Subtracts an offset from the pointer."""
        return self + -offset

    def __isub__(self, Py_ssize_t offset):
        """Subtracts an offset from the pointer in place."""
        return self.__iadd__(-offset)

    cpdef Py_ssize_t size(self):
        return self.mem.size - (self.ptr - self.mem.ptr)

    def __getbuffer__(self, Py_buffer *buffer, int flags):
        size = self.size()

        self._shape[0] = size
        self._strides[0] = 1

        buffer.buf = <void*>self.ptr
        buffer.format = 'b'
        buffer.internal = NULL
        buffer.itemsize = 1
        buffer.len = size
        buffer.ndim = 1
        buffer.obj = self
        buffer.readonly = 0
        buffer.shape = self._shape
        buffer.strides = self._strides
        buffer.suboffsets = NULL

    def __releasebuffer__(self, Py_buffer *buffer):
        pass

    def __getsegcount__(self, Py_ssize_t *lenp):
        if lenp != NULL:
            lenp[0] = self.size()
        return 1

    def __getreadbuffer__(self, Py_ssize_t idx, void **p):
        if idx != 0:
            raise SystemError("accessing non-existent buffer segment")
        p[0] = <void*>self.ptr
        return self.size()

    def __getwritebuffer__(self, Py_ssize_t idx, void **p):
        if idx != 0:
            raise SystemError("accessing non-existent buffer segment")
        p[0] = <void*>self.ptr
        return self.size()


cdef class _EventWatcher:
    cdef:
        cdef list events
        cdef object _lock

    def __init__(self):
        self.events = []
        self._lock = rlock.create_fastrlock()

    cpdef add(self, event, obj):
        """ Add event to be monitored.

        The ``obj`` are automatically released when the event done.

        Args:
            event (clpy.cuda.Event): The CUDA event to be monitored.
            obj: The object to be held.
        """
        rlock.lock_fastrlock(self._lock, -1, True)
        try:
            self._check_and_release_without_lock()
            if event.done:
                return
            self.events.append((event, obj))
        finally:
            rlock.unlock_fastrlock(self._lock)

    cpdef check_and_release(self):
        """ Check and release completed events.

        """
        if not self.events:
            return
        rlock.lock_fastrlock(self._lock, -1, True)
        try:
            self._check_and_release_without_lock()
        finally:
            rlock.unlock_fastrlock(self._lock)

    cpdef _check_and_release_without_lock(self):
        while self.events and self.events[0][0].done:
            del self.events[0]


cpdef PinnedMemoryPointer _malloc(Py_ssize_t size):
    mem = PinnedMemory(size)
    # # TODO(LWisteria): Use clEnqueueMapBuffer
    # mem = PinnedMemory(size, runtime.hostAllocPortable)
    return PinnedMemoryPointer(mem, 0)


cdef object _current_allocator = _malloc
cdef _EventWatcher _watcher = _EventWatcher()


cpdef _add_to_watch_list(event, obj):
    """ Add event to be monitored.

    The ``obj`` are automatically released when the event done.

    Args:
        event (clpy.cuda.Event): The CUDA event to be monitored.
        obj: The object to be held.
    """
    _watcher.add(event, obj)


cpdef PinnedMemoryPointer alloc_pinned_memory(Py_ssize_t size):
    """Calls the current allocator.

    Use :func:`~clpy.cuda.set_pinned_memory_allocator` to change the current
    allocator.

    Args:
        size (int): Size of the memory allocation.

    Returns:
        ~clpy.cuda.PinnedMemoryPointer: Pointer to the allocated buffer.

    """
    _watcher.check_and_release()
    return _current_allocator(size)


cpdef set_pinned_memory_allocator(allocator=_malloc):
    """Sets the current allocator.

    Args:
        allocator (function): CuPy pinned memory allocator. It must have the
            same interface as the :func:`clpy.cuda.alloc_alloc_pinned_memory`
            function, which takes the buffer size as an argument and returns
            the device buffer of that size.

    """
    global _current_allocator
    _current_allocator = allocator


class PooledPinnedMemory(PinnedMemory):

    """Memory allocation for a memory pool.

    As the instance of this class is created by memory pool allocator, users
    should not instantiate it manually.

    """

    def __init__(self, mem, pool):
        self.ptr = mem.ptr
        self.size = mem.size
        self.pool = pool

    def free(self):
        """Releases the memory buffer and sends it to the memory pool.

        This function actually does not free the buffer. It just returns the
        buffer to the memory pool for reuse.

        """
        pool = self.pool()
        if pool and self.ptr != 0:
            pool.free(self.ptr, self.size)
        self.ptr = 0
        self.size = 0

    __del__ = free


cdef class PinnedMemoryPool:

    """Memory pool on the host.

    Note that it preserves all allocated memory buffers even if the user
    explicitly release the one. Those released memory buffers are held by the
    memory pool as *free blocks*, and reused for further memory allocations of
    the same size.

    Args:
        allocator (function): The base CuPy pinned memory allocator. It is
            used for allocating new blocks when the blocks of the required
            size are all in use.

    """

    def __init__(self, allocator=_malloc):
        self._in_use = {}
        self._free = collections.defaultdict(list)
        self._alloc = allocator
        self._weakref = weakref.ref(self)
        self._lock = rlock.create_fastrlock()
        self._allocation_unit_size = 512

    cpdef PinnedMemoryPointer malloc(self, Py_ssize_t size):
        cdef list free

        if size == 0:
            return PinnedMemoryPointer(PinnedMemory(0), 0)

        # Round up the memory size to fit memory alignment of cudaHostAlloc
        unit = self._allocation_unit_size
        size = ((size + unit - 1) // unit) * unit
        rlock.lock_fastrlock(self._lock, -1, True)
        try:
            free = self._free[size]
            if free:
                mem = free.pop()
            else:
                # TODO(LWisteria): Use clEnqueueMapBuffer
                mem = self._alloc(size).mem
#                try:
#                    mem = self._alloc(size).mem
#                except runtime.CUDARuntimeError as e:
#                    if e.status != runtime.errorMemoryAllocation:
#                        raise
#                    self.free_all_blocks()
#                    mem = self._alloc(size).mem

            self._in_use[mem.ptr] = mem
        finally:
            rlock.unlock_fastrlock(self._lock)
        pmem = PooledPinnedMemory(mem, self._weakref)
        return PinnedMemoryPointer(pmem, 0)

    cpdef free(self, size_t ptr, Py_ssize_t size):
        cdef list free
        rlock.lock_fastrlock(self._lock, -1, True)
        try:
            mem = self._in_use.pop(ptr, None)
            if mem is None:
                raise RuntimeError('Cannot free out-of-pool memory')
            free = self._free[size]
            free.append(mem)
        finally:
            rlock.unlock_fastrlock(self._lock)

    cpdef free_all_blocks(self):
        """Release free all blocks."""
        rlock.lock_fastrlock(self._lock, -1, True)
        try:
            self._free.clear()
        finally:
            rlock.unlock_fastrlock(self._lock)

    cpdef n_free_blocks(self):
        """Count the total number of free blocks.

        Returns:
            int: The total number of free blocks.
        """
        cdef Py_ssize_t n = 0
        rlock.lock_fastrlock(self._lock, -1, True)
        try:
            for v in self._free.values():
                n += len(v)
        finally:
            rlock.unlock_fastrlock(self._lock)
        return n
