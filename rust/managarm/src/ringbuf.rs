//! Shared memory ring buffers; see `protocols/ringbuf/ringbuf.bragi` for the protocol
//! and `protocols/ringbuf/include/protocols/ringbuf/core.hpp` for the reference implementation.

use std::cell::{Cell, RefCell};
use std::future::poll_fn;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::task::{Poll, Waker};

use hel::{Handle, Mapping, MappingFlags};

bragi::include_binding!(mod bindings = "ringbuf.rs");

/// Offset of the data area within the producer memory.
pub const DATA_OFFSET: usize = 0x1000;
pub const CONSUMER_MEMORY_SIZE: usize = 0x1000;

/// Rights that a peer needs to map a memory object that it may write to.
pub const WRITER_RIGHTS: u32 =
    hel_sys::kHelRightRead | hel_sys::kHelRightWrite | hel_sys::kHelRightAssign;
/// Rights that a peer needs to map a memory object that it may not write to.
pub const READER_RIGHTS: u32 = hel_sys::kHelRightRead | hel_sys::kHelRightAssign;

const RECORD_HEADER_SIZE: usize = 8;
const RECORD_ALIGN: usize = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// The producer waits until the consumer frees up space.
    Reliable,
    /// The producer overwrites the oldest records and never waits.
    DropOldest,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Framing {
    Stream,
    Records,
}

#[derive(Debug, Clone, Copy)]
pub struct RingParameters {
    pub mode: Mode,
    pub framing: Framing,
    /// Size of the data area. Must be a power of two and a multiple of the page size.
    pub size: usize,
}

impl RingParameters {
    fn validate(&self) {
        assert!(self.size >= DATA_OFFSET && self.size.is_power_of_two());
        // Without record boundaries, consumers cannot recover from being overtaken.
        assert!(self.mode != Mode::DropOldest || self.framing == Framing::Records);
    }
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("the other side of the ring does not adhere to the protocol")]
    ProtocolViolation,
    #[error("the other side of the ring closed its kick lane")]
    PeerClosed,
    #[error("the descriptors that were received for the ring are unusable")]
    HandshakeFailed,
    #[error(transparent)]
    Hel(#[from] hel::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

/// Header of the producer memory. Only written by the producer.
#[repr(C)]
struct ProducerHeader {
    head: AtomicU64,
    tail: AtomicU64,
    flushed: AtomicU64,
    wake_at: AtomicU64,
}

/// Header of the consumer memory. Only written by the consumer.
#[repr(C)]
struct ConsumerHeader {
    consumed: AtomicU64,
    wake_at: AtomicU64,
}

const fn effective_record_size(record_size: usize) -> usize {
    (RECORD_HEADER_SIZE + record_size + RECORD_ALIGN - 1) & !(RECORD_ALIGN - 1)
}

fn passes_wake_at(wake_at: u64, old_position: u64, new_position: u64) -> bool {
    old_position < wake_at && wake_at <= new_position
}

// ----------------------------------------------------------------------------
// Kick lanes.
// ----------------------------------------------------------------------------

struct SenderState {
    lane: Handle,
    in_flight: Cell<bool>,
    again: Cell<bool>,
}

/// Offers Kick messages on a lane. Can be shared among multiple rings.
pub struct KickSender {
    state: Rc<SenderState>,
}

impl KickSender {
    pub fn new(lane: Handle) -> Rc<KickSender> {
        Rc::new(KickSender {
            state: Rc::new(SenderState {
                lane,
                in_flight: Cell::new(false),
                again: Cell::new(false),
            }),
        })
    }

    /// Never blocks; kicks that are issued while another kick is in flight are coalesced.
    pub fn kick(&self) {
        // The receiver may have examined the shared state before our latest change even if it
        // did not complete the exchange yet. Hence, we need to kick again in this case.
        if self.state.in_flight.get() {
            self.state.again.set(true);
            return;
        }
        self.state.in_flight.set(true);

        let state = self.state.clone();
        hel::spawn(async move {
            let head = bragi::head_to_bytes(&bindings::Kick::new())
                .expect("failed to serialize a Kick message");
            loop {
                state.again.set(false);
                let outcome =
                    hel::submit_async(&state.lane, hel::Offer::new((hel::SendBuffer::new(&head),)))
                        .await;
                // Errors mean that the receiver is gone; it will not miss the kick.
                let delivered = matches!(outcome, Ok((Ok(_), (Ok(_),))));
                if !delivered || !state.again.get() {
                    break;
                }
            }
            state.in_flight.set(false);
        });
    }
}

#[derive(Default)]
struct ReceiverState {
    sequence: Cell<u64>,
    closed: Cell<bool>,
    // Set when the KickReceiver is dropped.
    abandoned: Cell<bool>,
    wakers: RefCell<Vec<Waker>>,
}

impl ReceiverState {
    fn wake_all(&self) {
        for waker in self.wakers.borrow_mut().drain(..) {
            waker.wake();
        }
    }
}

/// Accepts Kick messages on a lane. Can be shared among multiple rings.
pub struct KickReceiver {
    state: Rc<ReceiverState>,
}

impl KickReceiver {
    pub fn new(lane: Handle) -> Rc<KickReceiver> {
        let state = Rc::new(ReceiverState::default());
        hel::spawn(Self::run(lane, state.clone()));
        Rc::new(KickReceiver { state })
    }

    async fn run(lane: Handle, state: Rc<ReceiverState>) {
        loop {
            let outcome =
                hel::submit_async(&lane, hel::Accept::new((hel::ReceiveInline,))).await;
            if state.abandoned.get() {
                return;
            }
            match outcome {
                // The contents of the message do not matter.
                Ok((Ok(_), _)) => {
                    state.sequence.set(state.sequence.get() + 1);
                    state.wake_all();
                }
                _ => break,
            }
        }
        state.closed.set(true);
        state.wake_all();
    }

    /// Number of kicks that were received so far.
    pub fn sequence(&self) -> u64 {
        self.state.sequence.get()
    }

    pub fn closed(&self) -> bool {
        self.state.closed.get()
    }

    /// Waits until `sequence()` changes or until the lane is closed.
    pub async fn wait_past(&self, sequence: u64) {
        poll_fn(|cx| {
            if self.state.sequence.get() != sequence || self.state.closed.get() {
                Poll::Ready(())
            } else {
                self.state.wakers.borrow_mut().push(cx.waker().clone());
                Poll::Pending
            }
        })
        .await
    }
}

impl Drop for KickReceiver {
    fn drop(&mut self) {
        // We cannot close the lane while the accept is in progress. Instead, run() terminates
        // on the next kick or when the peer is gone.
        self.state.abandoned.set(true);
    }
}

fn map_memory(memory: &Handle, size: usize, writable: bool) -> Result<(Mapping<u8>, NonNull<u8>)> {
    let mut flags = MappingFlags::READ;
    if writable {
        flags |= MappingFlags::WRITE;
    }
    // SAFETY: the mapping is only accessed through raw pointers and atomics.
    let mapping = unsafe { Mapping::<u8>::new(memory, None, 0, size, flags) }?;
    let pointer = unsafe { mapping.as_ptr() }.expect("mapping is not mapped");
    Ok((mapping, pointer))
}

// Copies that wrap around at the end of the data area.
unsafe fn copy_to_ring(data: NonNull<u8>, ring_size: usize, position: u64, source: &[u8]) {
    let offset = (position as usize) & (ring_size - 1);
    let pre_wrap = source.len().min(ring_size - offset);
    unsafe {
        std::ptr::copy_nonoverlapping(source.as_ptr(), data.as_ptr().add(offset), pre_wrap);
        std::ptr::copy_nonoverlapping(
            source.as_ptr().add(pre_wrap),
            data.as_ptr(),
            source.len() - pre_wrap,
        );
    }
}

unsafe fn copy_from_ring(data: NonNull<u8>, ring_size: usize, position: u64, target: &mut [u8]) {
    let offset = (position as usize) & (ring_size - 1);
    let pre_wrap = target.len().min(ring_size - offset);
    unsafe {
        std::ptr::copy_nonoverlapping(data.as_ptr().add(offset), target.as_mut_ptr(), pre_wrap);
        std::ptr::copy_nonoverlapping(
            data.as_ptr(),
            target.as_mut_ptr().add(pre_wrap),
            target.len() - pre_wrap,
        );
    }
}

// ----------------------------------------------------------------------------
// Producer.
// ----------------------------------------------------------------------------

struct ConsumerSlot {
    _memory: Handle,
    _mapping: Mapping<u8>,
    header: NonNull<ConsumerHeader>,
    sender: Rc<KickSender>,
}

pub struct Producer {
    params: RingParameters,
    _producer_memory: Handle,
    _producer_mapping: Mapping<u8>,
    header: NonNull<ProducerHeader>,
    data: NonNull<u8>,
    consumers: Vec<ConsumerSlot>,
    receiver: Option<Rc<KickReceiver>>,
    failed: bool,

    // The shared copies of our own positions are never read back.
    head: u64,
    tail: u64,
    flushed: u64,
    // Validated copy of the consumer's position.
    consumed: u64,
}

impl Producer {
    fn new(params: RingParameters, producer_memory: Handle) -> Result<Producer> {
        let (mapping, pointer) = map_memory(&producer_memory, DATA_OFFSET + params.size, true)?;
        Ok(Producer {
            params,
            _producer_memory: producer_memory,
            _producer_mapping: mapping,
            header: pointer.cast(),
            data: unsafe { pointer.add(DATA_OFFSET) },
            consumers: Vec::new(),
            receiver: None,
            failed: false,
            head: 0,
            tail: 0,
            flushed: 0,
            consumed: 0,
        })
    }

    fn attach_consumer(
        &mut self,
        consumer_memory: Handle,
        sender: Rc<KickSender>,
        receiver: Option<Rc<KickReceiver>>,
    ) -> Result<()> {
        let (mapping, pointer) = map_memory(&consumer_memory, CONSUMER_MEMORY_SIZE, false)?;
        if self.params.mode == Mode::Reliable {
            assert!(self.consumers.is_empty());
            self.receiver = receiver;
        }
        self.consumers.push(ConsumerSlot {
            _memory: consumer_memory,
            _mapping: mapping,
            header: pointer.cast(),
            sender,
        });
        Ok(())
    }

    fn header(&self) -> &ProducerHeader {
        unsafe { self.header.as_ref() }
    }

    /// Position of the next byte that is produced.
    pub fn head(&self) -> u64 {
        self.head
    }

    /// Base of the data area; the offset of a position is the position modulo the ring size.
    pub fn data_ptr(&self) -> NonNull<u8> {
        self.data
    }

    // Picks up the progress of the consumer.
    fn refresh(&mut self) -> Result<()> {
        assert!(self.params.mode == Mode::Reliable);
        let header = unsafe { self.consumers[0].header.as_ref() };
        // SeqCst since arm_space() relies on the ordering against the store to wake_at.
        let consumed = header.consumed.load(Ordering::SeqCst);
        if consumed < self.consumed || consumed > self.head {
            self.failed = true;
            return Err(Error::ProtocolViolation);
        }
        self.consumed = consumed;
        Ok(())
    }

    /// `Mode::Reliable` only: free space as of the last `wait_for_space()`.
    pub fn free_size(&self) -> usize {
        assert!(self.params.mode == Mode::Reliable);
        self.params.size - (self.head - self.consumed) as usize
    }

    /// `Mode::Reliable` only: commits bytes that were written to the data area at `head()`.
    pub fn produce(&mut self, size: usize) {
        assert!(size <= self.free_size());
        self.commit(self.head + size as u64);
    }

    /// `Framing::Stream` only: copies as many bytes as possible into the free space.
    pub fn write(&mut self, data: &[u8]) -> usize {
        assert!(self.params.framing == Framing::Stream);
        let chunk = data.len().min(self.free_size());
        unsafe { copy_to_ring(self.data, self.params.size, self.head, &data[..chunk]) };
        self.produce(chunk);
        chunk
    }

    fn commit(&mut self, new_head: u64) {
        let old_head = self.head;
        self.head = new_head;
        // Commit the operation *after* writing to the ring.
        self.header().head.store(new_head, Ordering::SeqCst);

        for slot in &self.consumers {
            let wake_at = unsafe { slot.header.as_ref() }.wake_at.load(Ordering::SeqCst);
            if passes_wake_at(wake_at, old_head, new_head) {
                slot.sender.kick();
            }
        }
    }

    fn write_record(&mut self, record: &[u8]) {
        let mut record_header = [0u8; RECORD_HEADER_SIZE];
        record_header[..4].copy_from_slice(&(record.len() as u32).to_ne_bytes());
        unsafe {
            copy_to_ring(self.data, self.params.size, self.head, &record_header);
            copy_to_ring(
                self.data,
                self.params.size,
                self.head + RECORD_HEADER_SIZE as u64,
                record,
            );
        }
        self.commit(self.head + effective_record_size(record.len()) as u64);
    }

    /// `Mode::Reliable` only: waits until the given amount of space is free.
    pub async fn wait_for_space(&mut self, size: usize) -> Result<()> {
        assert!(size <= self.params.size);
        let receiver = self.receiver.clone().expect("ring has no consumer");
        loop {
            if self.failed {
                return Err(Error::ProtocolViolation);
            }

            let sequence = receiver.sequence();
            self.refresh()?;
            if self.free_size() >= size {
                return Ok(());
            }
            let wake_at = self.head + size as u64 - self.params.size as u64;
            self.header().wake_at.store(wake_at, Ordering::SeqCst);
            self.refresh()?;
            if self.free_size() >= size {
                return Ok(());
            }

            if receiver.closed() {
                return Err(Error::PeerClosed);
            }
            receiver.wait_past(sequence).await;
        }
    }

    /// `Framing::Records` only. `Mode::Reliable`: waits until space is available.
    /// `Mode::DropOldest`: completes immediately.
    pub async fn enqueue(&mut self, record: &[u8]) -> Result<()> {
        if self.params.mode == Mode::DropOldest {
            self.enqueue_now(record);
            return Ok(());
        }
        assert!(self.params.framing == Framing::Records);
        self.wait_for_space(effective_record_size(record.len()))
            .await?;
        self.write_record(record);
        Ok(())
    }

    /// `Mode::DropOldest` only: overwrites the oldest records if necessary.
    pub fn enqueue_now(&mut self, record: &[u8]) {
        assert!(self.params.framing == Framing::Records);
        assert!(self.params.mode == Mode::DropOldest);
        let ring_size = self.params.size as u64;
        let effective_size = effective_record_size(record.len()) as u64;
        assert!(effective_size <= ring_size);

        // Compute the invalidated part of the ring buffer.
        let mut invalidated = self.tail;
        while invalidated + ring_size < self.head + effective_size {
            assert!(invalidated < self.head);
            let mut tail_header = [0u8; RECORD_HEADER_SIZE];
            unsafe { copy_from_ring(self.data, self.params.size, invalidated, &mut tail_header) };
            let tail_size = u32::from_ne_bytes(tail_header[..4].try_into().unwrap());
            invalidated += effective_record_size(tail_size as usize) as u64;
        }

        // Invalidate the ring *before* writing to it.
        self.tail = invalidated;
        self.header().tail.store(invalidated, Ordering::SeqCst);

        self.write_record(record);
    }

    /// Asks the consumers to process all committed data regardless of their watermarks.
    pub fn flush(&mut self) {
        if self.flushed == self.head {
            return;
        }
        self.flushed = self.head;
        self.header().flushed.store(self.flushed, Ordering::SeqCst);

        for slot in &self.consumers {
            let wake_at = unsafe { slot.header.as_ref() }.wake_at.load(Ordering::SeqCst);
            if wake_at > self.head {
                slot.sender.kick();
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Consumer.
// ----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct Record {
    /// Size of the record; the record is truncated if this is larger than the buffer.
    pub size: usize,
    /// `Mode::DropOldest` only: number of bytes that were overwritten before they were dequeued.
    pub lost: u64,
}

pub struct Consumer {
    params: RingParameters,
    _producer_memory: Handle,
    _consumer_memory: Handle,
    _producer_mapping: Mapping<u8>,
    _consumer_mapping: Mapping<u8>,
    producer_header: NonNull<ProducerHeader>,
    header: NonNull<ConsumerHeader>,
    data: NonNull<u8>,
    receiver: Option<Rc<KickReceiver>>,
    sender: Option<Rc<KickSender>>,
    failed: bool,

    // The shared copy of our own position is never read back.
    position: u64,
    // Validated copy of the producer's position.
    head: u64,
    // Mode::DropOldest only: lost bytes that were not reported yet.
    lost: u64,
}

impl Consumer {
    fn new(
        params: RingParameters,
        producer_memory: Handle,
        consumer_memory: Handle,
    ) -> Result<Consumer> {
        let (producer_mapping, producer_pointer) =
            map_memory(&producer_memory, DATA_OFFSET + params.size, false)?;
        let (consumer_mapping, consumer_pointer) =
            map_memory(&consumer_memory, CONSUMER_MEMORY_SIZE, true)?;
        Ok(Consumer {
            params,
            _producer_memory: producer_memory,
            _consumer_memory: consumer_memory,
            _producer_mapping: producer_mapping,
            _consumer_mapping: consumer_mapping,
            producer_header: producer_pointer.cast(),
            header: consumer_pointer.cast(),
            data: unsafe { producer_pointer.add(DATA_OFFSET) },
            receiver: None,
            sender: None,
            failed: false,
            position: 0,
            head: 0,
            lost: 0,
        })
    }

    fn producer_header(&self) -> &ProducerHeader {
        unsafe { self.producer_header.as_ref() }
    }

    fn header(&self) -> &ConsumerHeader {
        unsafe { self.header.as_ref() }
    }

    /// Position of the next byte that is consumed.
    pub fn position(&self) -> u64 {
        self.position
    }

    /// Base of the data area; the offset of a position is the position modulo the ring size.
    pub fn data_ptr(&self) -> NonNull<u8> {
        self.data
    }

    // Picks up the progress of the producer.
    fn refresh(&mut self) -> Result<()> {
        assert!(self.params.mode == Mode::Reliable);
        let head = self.producer_header().head.load(Ordering::SeqCst);
        if head < self.head || head > self.position + self.params.size as u64 {
            self.failed = true;
            return Err(Error::ProtocolViolation);
        }
        self.head = head;
        Ok(())
    }

    /// `Mode::Reliable` only: number of available bytes as of the last `wait_for_data()`.
    pub fn available_size(&self) -> usize {
        assert!(self.params.mode == Mode::Reliable);
        (self.head - self.position) as usize
    }

    /// `Mode::Reliable` only: frees bytes that were taken from the data area at `position()`.
    pub fn consume(&mut self, size: usize) {
        assert!(size <= self.available_size());
        let old_position = self.position;
        self.position += size as u64;
        self.header().consumed.store(self.position, Ordering::SeqCst);

        let wake_at = self.producer_header().wake_at.load(Ordering::SeqCst);
        if passes_wake_at(wake_at, old_position, self.position) {
            self.sender
                .as_ref()
                .expect("ring has no producer")
                .kick();
        }
    }

    /// `Framing::Stream` only: copies as many available bytes as possible to the buffer.
    pub fn read(&mut self, buffer: &mut [u8]) -> usize {
        assert!(self.params.framing == Framing::Stream);
        let chunk = buffer.len().min(self.available_size());
        unsafe { copy_from_ring(self.data, self.params.size, self.position, &mut buffer[..chunk]) };
        self.consume(chunk);
        chunk
    }

    /// Waits until `watermark` bytes are available or until the producer flushes.
    pub async fn wait_for_data(&mut self, watermark: usize) -> Result<()> {
        assert!(watermark > 0 && watermark <= self.params.size);
        let receiver = self.receiver.clone().expect("ring has no producer");
        loop {
            if self.failed {
                return Err(Error::ProtocolViolation);
            }

            let sequence = receiver.sequence();
            let wake_at = self.position + watermark as u64;
            self.header().wake_at.store(wake_at, Ordering::SeqCst);

            let mut head = self.producer_header().head.load(Ordering::SeqCst);
            if self.params.mode == Mode::Reliable {
                self.refresh()?;
                head = self.head;
            }
            if head >= wake_at {
                return Ok(());
            }
            if self.producer_header().flushed.load(Ordering::SeqCst) > self.position {
                return Ok(());
            }

            if receiver.closed() {
                return Err(Error::PeerClosed);
            }
            receiver.wait_past(sequence).await;
        }
    }

    /// `Framing::Records` only: copies the next record to the buffer.
    /// Returns `None` if the ring is empty.
    pub fn try_dequeue(&mut self, buffer: &mut [u8]) -> Result<Option<Record>> {
        assert!(self.params.framing == Framing::Records);
        if self.failed {
            return Err(Error::ProtocolViolation);
        }
        let outcome = match self.params.mode {
            Mode::Reliable => self.dequeue_reliable(buffer),
            Mode::DropOldest => self.dequeue_drop_oldest(buffer),
        };
        if outcome.is_err() {
            self.failed = true;
        }
        outcome
    }

    fn read_record_header(&self, position: u64) -> usize {
        let mut record_header = [0u8; RECORD_HEADER_SIZE];
        unsafe { copy_from_ring(self.data, self.params.size, position, &mut record_header) };
        u32::from_ne_bytes(record_header[..4].try_into().unwrap()) as usize
    }

    fn dequeue_reliable(&mut self, buffer: &mut [u8]) -> Result<Option<Record>> {
        self.refresh()?;
        if self.available_size() == 0 {
            return Ok(None);
        }
        if self.available_size() < RECORD_HEADER_SIZE {
            return Err(Error::ProtocolViolation);
        }

        let record_size = self.read_record_header(self.position);
        if record_size > self.params.size || effective_record_size(record_size) > self.available_size()
        {
            return Err(Error::ProtocolViolation);
        }
        let chunk = record_size.min(buffer.len());
        unsafe {
            copy_from_ring(
                self.data,
                self.params.size,
                self.position + RECORD_HEADER_SIZE as u64,
                &mut buffer[..chunk],
            )
        };

        self.consume(effective_record_size(record_size));
        Ok(Some(Record {
            size: record_size,
            lost: 0,
        }))
    }

    fn dequeue_drop_oldest(&mut self, buffer: &mut [u8]) -> Result<Option<Record>> {
        loop {
            // Find a valid position to dequeue from.
            let before_tail = self.producer_header().tail.load(Ordering::SeqCst);
            if self.position < before_tail {
                self.lost += before_tail - self.position;
                self.position = before_tail;
            }

            let head = self.producer_header().head.load(Ordering::SeqCst);
            if self.position == head {
                return Ok(None);
            }
            if self.position > head || head - self.position < RECORD_HEADER_SIZE as u64 {
                return Err(Error::ProtocolViolation);
            }

            // The size is garbage if the record is overwritten concurrently.
            let record_size = self.read_record_header(self.position);
            let sane_size = record_size <= self.params.size
                && effective_record_size(record_size) as u64 <= head - self.position;
            if sane_size {
                let chunk = record_size.min(buffer.len());
                unsafe {
                    copy_from_ring(
                        self.data,
                        self.params.size,
                        self.position + RECORD_HEADER_SIZE as u64,
                        &mut buffer[..chunk],
                    )
                };
            }

            // Validate the data *after* copying.
            let after_tail = self.producer_header().tail.load(Ordering::SeqCst);
            if self.position < after_tail {
                continue;
            }
            if !sane_size {
                return Err(Error::ProtocolViolation);
            }

            self.position += effective_record_size(record_size) as u64;
            return Ok(Some(Record {
                size: record_size,
                lost: std::mem::take(&mut self.lost),
            }));
        }
    }

    /// `Framing::Records` only: waits until a record is available and copies it to the buffer.
    pub async fn dequeue(&mut self, buffer: &mut [u8]) -> Result<Record> {
        loop {
            if let Some(record) = self.try_dequeue(buffer)? {
                return Ok(record);
            }
            self.wait_for_data(1).await?;
        }
    }
}

// ----------------------------------------------------------------------------
// receive().
// ----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Producer,
    Consumer,
}

/// Configuration of a ring and the role that the side that calls `receive()` plays for it.
#[derive(Debug, Clone, Copy)]
pub struct RingSpec {
    pub params: RingParameters,
    pub role: Role,
}

/// A ring that was connected by `receive()`.
pub enum Ring {
    Producer(Producer),
    Consumer(Consumer),
}

impl Ring {
    pub fn into_producer(self) -> Producer {
        match self {
            Ring::Producer(producer) => producer,
            Ring::Consumer(_) => panic!("ring is not a producer"),
        }
    }

    pub fn into_consumer(self) -> Consumer {
        match self {
            Ring::Consumer(consumer) => consumer,
            Ring::Producer(_) => panic!("ring is not a consumer"),
        }
    }
}

// Rights that are required to exchange messages over a lane.
const LANE_RIGHTS: u32 = hel_sys::kHelRightInvoke;

// Determines which kick lanes are required. Roles are the roles of the side that calls receive().
// Returns (provider offers kicks, receiver offers kicks).
fn lane_needs(specs: impl Iterator<Item = (Mode, Role)>) -> (bool, bool) {
    let mut to_receiver = false;
    let mut to_provider = false;
    for (mode, role) in specs {
        // Consumers always wait; producers only wait for Mode::Reliable.
        let producer_waits = mode == Mode::Reliable;
        match role {
            Role::Consumer => {
                to_receiver = true;
                to_provider |= producer_waits;
            }
            Role::Producer => {
                to_provider = true;
                to_receiver |= producer_waits;
            }
        }
    }
    (to_receiver, to_provider)
}

async fn pull_descriptor(conversation: &Handle, rights: u32) -> Result<Handle> {
    hel::submit_async(conversation, hel::PullDescriptor::new(rights))
        .await??
        .ok_or(Error::HandshakeFailed)
}

// Do not trust the peer to pass memory objects of the correct size.
fn check_memory_size(memory: &Handle, size: usize) -> Result<()> {
    let mut actual_size = 0;
    let error = unsafe { hel_sys::helMemoryInfo(memory.handle(), &mut actual_size) };
    if error as u32 != hel_sys::kHelErrNone || actual_size < size {
        return Err(Error::HandshakeFailed);
    }
    Ok(())
}

/// Pulls the descriptors of the rings that the peer provides from a conversation lane.
/// Both sides need to agree on the order and the configuration of the rings.
pub async fn receive(conversation: &Handle, specs: &[RingSpec]) -> Result<Vec<Ring>> {
    for spec in specs {
        spec.params.validate();
    }
    let (to_receiver, to_provider) =
        lane_needs(specs.iter().map(|spec| (spec.params.mode, spec.role)));

    let mut receiver = None;
    let mut sender = None;
    if to_receiver {
        receiver = Some(KickReceiver::new(
            pull_descriptor(conversation, LANE_RIGHTS).await?,
        ));
    }
    if to_provider {
        sender = Some(KickSender::new(
            pull_descriptor(conversation, LANE_RIGHTS).await?,
        ));
    }

    let mut rings = Vec::new();
    for spec in specs {
        let (producer_rights, consumer_rights) = match spec.role {
            Role::Producer => (WRITER_RIGHTS, READER_RIGHTS),
            Role::Consumer => (READER_RIGHTS, WRITER_RIGHTS),
        };
        let producer_memory = pull_descriptor(conversation, producer_rights).await?;
        let consumer_memory = pull_descriptor(conversation, consumer_rights).await?;
        check_memory_size(&producer_memory, DATA_OFFSET + spec.params.size)?;
        check_memory_size(&consumer_memory, CONSUMER_MEMORY_SIZE)?;

        rings.push(match spec.role {
            Role::Producer => {
                let mut producer = Producer::new(spec.params, producer_memory)?;
                producer.attach_consumer(
                    consumer_memory,
                    sender.clone().expect("producers always send kicks"),
                    receiver.clone(),
                )?;
                Ring::Producer(producer)
            }
            Role::Consumer => {
                let mut consumer = Consumer::new(spec.params, producer_memory, consumer_memory)?;
                consumer.receiver = receiver.clone();
                consumer.sender = sender.clone();
                Ring::Consumer(consumer)
            }
        });
    }
    Ok(rings)
}
