use std::marker::PhantomData;
use std::ptr::NonNull;

use uacpi_sys::{
    uacpi_resource, uacpi_resource_address_common, uacpi_resource_address32,
    uacpi_resource_address64, uacpi_resource_extended_irq, uacpi_resource_fixed_io,
    uacpi_resource_fixed_memory32, uacpi_resource_io, uacpi_resource_irq, uacpi_resource_memory32,
    uacpi_resource_type, uacpi_resources,
};

use super::namespace::NamespaceNode;
use super::{Result, check};

/// Size of the type and length fields that precede every resource.
const RESOURCE_HEADER_SIZE: usize = 8;

/// Determines how an interrupt is triggered.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Triggering {
    Edge,
    Level,
}

impl Triggering {
    fn from_raw(triggering: u8) -> Triggering {
        if u32::from(triggering) == uacpi_sys::UACPI_TRIGGERING_EDGE {
            Triggering::Edge
        } else {
            Triggering::Level
        }
    }
}

/// Determines the polarity that an interrupt is asserted with.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Polarity {
    ActiveHigh,
    ActiveLow,
    ActiveBoth,
}

impl Polarity {
    fn from_raw(polarity: u8) -> Polarity {
        match u32::from(polarity) {
            uacpi_sys::UACPI_POLARITY_ACTIVE_HIGH => Polarity::ActiveHigh,
            uacpi_sys::UACPI_POLARITY_ACTIVE_BOTH => Polarity::ActiveBoth,
            _ => Polarity::ActiveLow,
        }
    }
}

/// An IRQ resource, i.e., a resource of type UACPI_RESOURCE_TYPE_IRQ.
#[derive(Clone, Copy)]
pub struct Irq<'a> {
    irq: &'a uacpi_resource_irq,
}

impl<'a> Irq<'a> {
    pub fn triggering(self) -> Triggering {
        Triggering::from_raw(self.irq.triggering)
    }

    pub fn polarity(self) -> Polarity {
        Polarity::from_raw(self.irq.polarity)
    }

    /// Returns the ISA IRQs that the resource can be connected to.
    pub fn irqs(self) -> &'a [u8] {
        // SAFETY: the resource is followed by num_irqs IRQs.
        unsafe { self.irq.irqs.as_slice(self.irq.num_irqs.into()) }
    }
}

/// An extended IRQ resource, i.e., a resource of type UACPI_RESOURCE_TYPE_EXTENDED_IRQ.
#[derive(Clone, Copy)]
pub struct ExtendedIrq<'a> {
    irq: &'a uacpi_resource_extended_irq,
}

impl<'a> ExtendedIrq<'a> {
    pub fn triggering(self) -> Triggering {
        Triggering::from_raw(self.irq.triggering)
    }

    pub fn polarity(self) -> Polarity {
        Polarity::from_raw(self.irq.polarity)
    }

    /// Returns the global IRQs that the resource can be connected to.
    pub fn irqs(self) -> &'a [u32] {
        // SAFETY: the resource is followed by num_irqs IRQs.
        unsafe { self.irq.irqs.as_slice(self.irq.num_irqs.into()) }
    }
}

/// An IO port range resource, i.e., a resource of type UACPI_RESOURCE_TYPE_IO.
#[derive(Clone, Copy)]
pub struct Io<'a> {
    io: &'a uacpi_resource_io,
}

impl Io<'_> {
    pub fn minimum(self) -> u16 {
        self.io.minimum
    }

    pub fn maximum(self) -> u16 {
        self.io.maximum
    }

    /// Number of ports of the range.
    pub fn length(self) -> u8 {
        self.io.length
    }
}

/// A fixed IO port range resource, i.e., a resource of type UACPI_RESOURCE_TYPE_FIXED_IO.
#[derive(Clone, Copy)]
pub struct FixedIo<'a> {
    io: &'a uacpi_resource_fixed_io,
}

impl FixedIo<'_> {
    pub fn address(self) -> u16 {
        self.io.address
    }

    pub fn length(self) -> u8 {
        self.io.length
    }
}

/// A memory range resource, i.e., a resource of type UACPI_RESOURCE_TYPE_FIXED_MEMORY32,
/// UACPI_RESOURCE_TYPE_MEMORY32 or a memory UACPI_RESOURCE_TYPE_ADDRESS32/64.
#[derive(Clone, Copy)]
pub struct Memory {
    address: u64,
    length: u64,
}

impl Memory {
    pub fn address(self) -> u64 {
        self.address
    }

    pub fn length(self) -> u64 {
        self.length
    }
}

/// A single resource of a [`Resources`] list.
pub enum Resource<'a> {
    Irq(Irq<'a>),
    ExtendedIrq(ExtendedIrq<'a>),
    Io(Io<'a>),
    FixedIo(FixedIo<'a>),
    Memory(Memory),
    /// A resource of a type that we do not wrap yet, i.e., a UACPI_RESOURCE_TYPE_* value.
    Other(uacpi_resource_type),
}

/// Whether an address space resource describes memory (and not IO ports or bus numbers).
fn is_memory_range(common: &uacpi_resource_address_common) -> bool {
    u32::from(common.type_) == uacpi_sys::UACPI_RANGE_MEMORY
}

/// A list of resources that uACPI allocated for us.
pub struct Resources {
    resources: NonNull<uacpi_resources>,
}

impl NamespaceNode {
    /// Wraps uacpi_get_current_resources(), i.e., evaluates _CRS.
    pub fn current_resources(self) -> Result<Resources> {
        let mut resources: *mut uacpi_resources = std::ptr::null_mut();
        // SAFETY: uACPI only writes the pointer to the list that it allocates.
        let status =
            unsafe { uacpi_sys::uacpi_get_current_resources(self.as_raw(), &mut resources) };
        check("uacpi_get_current_resources", status)?;

        // uACPI does not hand out lists that it failed to allocate.
        let resources = NonNull::new(resources).expect("uACPI returned no resources");
        Ok(Resources { resources })
    }
}

impl Resources {
    /// Iterates the resources of the list.
    pub fn iter(&self) -> ResourceIter<'_> {
        // SAFETY: we hold the only reference to the list.
        let resources = unsafe { self.resources.as_ref() };
        ResourceIter {
            cur: resources.entries,
            remaining: resources.length,
            marker: PhantomData,
        }
    }
}

impl Drop for Resources {
    /// Wraps uacpi_free_resources().
    fn drop(&mut self) {
        // SAFETY: we free the list that we obtained in current_resources().
        unsafe { uacpi_sys::uacpi_free_resources(self.resources.as_ptr()) };
    }
}

/// Iterator over the resources of a [`Resources`] list.
pub struct ResourceIter<'a> {
    cur: *mut uacpi_resource,
    remaining: usize,
    marker: PhantomData<&'a Resources>,
}

impl<'a> Iterator for ResourceIter<'a> {
    type Item = Resource<'a>;

    fn next(&mut self) -> Option<Resource<'a>> {
        if self.remaining < RESOURCE_HEADER_SIZE {
            return None;
        }

        // Resources are variable length, hence only the header is known to be present.
        // SAFETY: the header lies within the list.
        let (type_, length) = unsafe {
            (
                (&raw const (*self.cur).type_).read(),
                (&raw const (*self.cur).length).read() as usize,
            )
        };
        if type_ == uacpi_sys::UACPI_RESOURCE_TYPE_END_TAG {
            return None;
        }

        // Give up instead of running past the end of a list that we cannot make sense of.
        if length < RESOURCE_HEADER_SIZE || length > self.remaining {
            println!("sif: Ignoring resource with a length of {length} bytes");
            return None;
        }

        // SAFETY: the payload lies within the list and the type tells us how to interpret it.
        let resource = unsafe {
            let payload = &raw const (*self.cur).__bindgen_anon_1;
            match type_ {
                uacpi_sys::UACPI_RESOURCE_TYPE_IRQ => Resource::Irq(Irq {
                    irq: &*payload.cast::<uacpi_resource_irq>(),
                }),
                uacpi_sys::UACPI_RESOURCE_TYPE_EXTENDED_IRQ => Resource::ExtendedIrq(ExtendedIrq {
                    irq: &*payload.cast::<uacpi_resource_extended_irq>(),
                }),
                uacpi_sys::UACPI_RESOURCE_TYPE_IO => Resource::Io(Io {
                    io: &*payload.cast::<uacpi_resource_io>(),
                }),
                uacpi_sys::UACPI_RESOURCE_TYPE_FIXED_IO => Resource::FixedIo(FixedIo {
                    io: &*payload.cast::<uacpi_resource_fixed_io>(),
                }),
                uacpi_sys::UACPI_RESOURCE_TYPE_FIXED_MEMORY32 => {
                    let memory = &*payload.cast::<uacpi_resource_fixed_memory32>();
                    Resource::Memory(Memory {
                        address: memory.address.into(),
                        length: memory.length.into(),
                    })
                }
                uacpi_sys::UACPI_RESOURCE_TYPE_MEMORY32 => {
                    let memory = &*payload.cast::<uacpi_resource_memory32>();
                    Resource::Memory(Memory {
                        address: memory.minimum.into(),
                        length: memory.length.into(),
                    })
                }
                uacpi_sys::UACPI_RESOURCE_TYPE_ADDRESS32 => {
                    let address = &*payload.cast::<uacpi_resource_address32>();
                    if is_memory_range(&address.common) {
                        Resource::Memory(Memory {
                            address: u64::from(address.minimum)
                                + u64::from(address.translation_offset),
                            length: address.address_length.into(),
                        })
                    } else {
                        Resource::Other(type_)
                    }
                }
                uacpi_sys::UACPI_RESOURCE_TYPE_ADDRESS64 => {
                    let address = &*payload.cast::<uacpi_resource_address64>();
                    if is_memory_range(&address.common) {
                        Resource::Memory(Memory {
                            address: address.minimum.wrapping_add(address.translation_offset),
                            length: address.address_length,
                        })
                    } else {
                        Resource::Other(type_)
                    }
                }
                _ => Resource::Other(type_),
            }
        };

        // SAFETY: the next resource starts at the end of the current one, within the list.
        self.cur = unsafe { self.cur.cast::<u8>().add(length).cast::<uacpi_resource>() };
        self.remaining -= length;

        Some(resource)
    }
}
