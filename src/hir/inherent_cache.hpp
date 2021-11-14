/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/inherent_cache.hpp
 * - Inherent method lookup cache
 */
#pragma once
#include "../include/range_vec_map.hpp"
#include "type_ref.hpp"
#include <map>

namespace HIR {

class TypeImpl;

/// <summary>
/// Cached lookup logic for inherent (non-trait) methods on types
/// </summary>
class InherentCache
{
private:
	typedef ::std::function<void(const HIR::TypeRef& self_ty, const HIR::TypeImpl& impl)>	inner_callback_t;
	struct Lowest
	{
		// Same as HIR::Crate::ImplGroup
		typedef ::std::vector<const HIR::TypeImpl*>	list_t;
		::std::map<::HIR::SimplePath, list_t>   named;
		list_t  non_named; // TODO: use a map of HIR::TypeRef::Data::Tag
		list_t  generic;

		void insert(const Span& sp, const HIR::TypeImpl& impl);
		void iterate(const HIR::TypeRef& ty, inner_callback_t& cb) const;
	};

	/// <summary>
	/// A layer of the cache
	/// </summary>
	struct Inner
	{
		/// Cache content used for just `Self` 
		Lowest	m_byvalue;
		// Sub-caches for different wrappers around `Self` (can recurse)
		std::unique_ptr<Inner>	m_ref;
		std::unique_ptr<Inner>	m_ref_mut;
		std::unique_ptr<Inner>	m_ref_move;
		std::unique_ptr<Inner>	m_ptr;
		std::unique_ptr<Inner>	m_ptr_mut;
		std::unique_ptr<Inner>	m_ptr_move;
		std::map<HIR::SimplePath,Inner>	m_path;

		void insert(const Span& sp, const HIR::TypeRef& receiver, const HIR::TypeImpl& impl);
		void find(const Span& sp, const HIR::TypeRef& cur_ty, t_cb_resolve_type ty_res, inner_callback_t& cb) const;
	};

	std::map<RcString,Inner>	items;

public:
	/// Callback arguments:
	/// `self_ty`: Type for `Self` within the `impl` block
	/// `impl`: TypeImpl containing this method
	typedef ::std::function<void(const HIR::TypeRef& self_ty, const HIR::TypeImpl& impl)>	callback_t;

	void insert_all(const Span& sp, const HIR::TypeImpl& impl, const HIR::SimplePath& lang_Box);
	/// Locates methods matching the specifided type
	void find(const Span& sp, const RcString& name, const HIR::TypeRef& ty, t_cb_resolve_type ty_res, callback_t cb) const;
};

}