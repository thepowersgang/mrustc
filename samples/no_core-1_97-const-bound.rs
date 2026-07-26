#![allow(internal_features)]
#![feature(no_core, lang_items)]
#![no_core]

#[lang = "sized"]
trait Sized: MetaSized {}

#[lang = "meta_sized"]
trait MetaSized: PointeeSized {}

#[lang = "pointee_sized"]
trait PointeeSized {}

trait ZeroablePrimitive {}
trait PartialEq {}
trait Copy {}
const trait TruncateTarget<Target> {}
const unsafe trait TrivialClone: [const] PartialEq {}
const trait BorrowMut<T>: [const] PartialEq + const Copy + ~const ZeroablePrimitive {}

trait GenericBound<T: [const] PartialEq> {}

fn accepts_const_trait_bound<T>()
where
    T: ZeroablePrimitive + [const] PartialEq + const Copy + ~const TruncateTarget<T>,
{
}

fn accepts_impl_const_trait_bound(value: impl [const] PartialEq + [const] Copy) {}
