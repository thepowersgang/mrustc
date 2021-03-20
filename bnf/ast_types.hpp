#pragma once

#include <vector>
#include <memory>
#include <utility>
#include <cassert>

template<typename T>
T consume(T* ptr) {
	T rv = ::std::move(*ptr);
	delete ptr;
	return rv;
}
/// Stick `v` into a raw pointer
template<typename T>
T* new_(T v) {
	return new T(std::move(v));
}
template<typename T>
::std::unique_ptr<T> box_raw(T *ptr) {
	return ::std::unique_ptr<T>(ptr);
}
template<typename T>
::std::unique_ptr<T> box(T&& ptr) {
	return ::std::unique_ptr<T>(new T(::std::move(ptr)));
}

class ItemPath
{
public:
	static ItemPath new_relative(std::string base)
	{
		return ItemPath();
	}
	static ItemPath new_self()
	{
		return ItemPath();
	}
	static ItemPath new_super()
	{
		return ItemPath();
	}
	static ItemPath new_abs()
	{
		return ItemPath();
	}
	static ItemPath new_crate()
	{
		return ItemPath();
	}

	void push(std::string v) {
	}
};


class Path
{
public:
	struct TagSelf {};
	Path(TagSelf)
	{}
	struct TagSuper {};
	Path(TagSuper)
	{}
	struct TagAbs {};
	Path(TagAbs)
	{}
};

class TokenTree;
typedef ::std::vector<TokenTree>	TokenTreeList;

class TokenTree
{
	int	m_tok;
	TokenTreeList	m_subtts;
public:
	TokenTree(TokenTreeList tts):
		m_tok(0),
		m_subtts(tts)
	{}
	TokenTree(int tok):
		m_tok(tok)
	{}
};

class MetaItem;
typedef ::std::vector<MetaItem>	MetaItems;

class MetaItem
{
	::std::string	m_key;
	enum class Type {
		Empty,
		String,
		Tree
	} m_type;
	::std::string	m_value;
	TokenTree	m_sub_items;
	// TODO: How to represent `#[attr()]` (i.e. empty paren set) as distinct from `#[attr=""]` and `#[attr]`
public:
	MetaItem(::std::string key):
		m_key(key),
		m_type(Type::Empty),
		m_sub_items(0)
	{}
	MetaItem(::std::string key, ::std::string value):
		m_key(key),
		m_type(Type::String),
		m_value(value),
		m_sub_items(0)
	{}
	MetaItem(::std::string key, TokenTree sub_items):
		m_key(key),
		m_type(Type::Tree),
		m_sub_items(std::move(sub_items))
	{}

	const ::std::string& key() const { return m_key; }
	const ::std::string& string() const { assert(m_type == Type::String); return m_value; }
};
class AttrList
{
	::std::vector<MetaItem>	m_ents;
public:
	bool has(const char* name) const {
		return get_first_ptr(name) != nullptr;
	}
	bool has(const ::std::string& name) const {
		return this->has(name.c_str());
	}

	const MetaItem* get_first_ptr(const char* name) const {
		for(const auto& e : m_ents) {
			if( e.key() == name)
				return &e;
		}
		return nullptr;
	}
	const MetaItem& get_first(const char* name) const {
		auto p = get_first_ptr(name);
		assert(p != 0);
		return *p;
	}

	void push_back(MetaItem a) {
		m_ents.push_back( ::std::move(a) );
	}
	void append(AttrList other) {
		for(auto& e : other.m_ents)
			m_ents.push_back( ::std::move(e) );
	}
};




class MacroInv
{
	ItemPath	m_name;
	::std::string	m_ident;
	TokenTree	m_args;
public:
	MacroInv(ItemPath ip, TokenTree contents):
		m_name(std::move(ip)),
		m_ident(),
		m_args(contents)
	{}
	MacroInv(ItemPath ip, ::std::string ident, TokenTree contents):
		m_name( std::move(ip) ),
		m_ident(ident),
		m_args(contents)
	{}
};

class Item
{
	bool	m_is_pub;
protected:
	AttrList	m_attrs;
public:
	Item():
		m_is_pub(false)
	{}
	Item(AttrList attrs):
		m_attrs(attrs)
	{}
	virtual ~Item() {
	}

	void set_pub(bool v) {
		m_is_pub = v;
	}
	void add_attrs(AttrList a) {
		m_attrs.append( ::std::move(a) );
	}

	const AttrList& attrs() const { return m_attrs; }
};

typedef ::std::vector< ::std::unique_ptr<Item> >	ItemList;

class Global:
	public Item
{
};
class Module:
	public Item
{
	bool	m_is_extern;
	::std::string	m_name;
	ItemList	m_items;

	::std::vector<::std::string>	m_mod_path;
	::std::string	m_filename, m_base_dir;
public:
	Module(Module&&) = default;
	Module(::std::string name):
		m_is_extern(true),
		m_name(name)
	{}
	Module(AttrList attrs, ::std::vector< ::std::unique_ptr<Item> >	items):
		Item(attrs),
		m_is_extern(false),
		m_items( ::std::move(items) )
	{}
	Module& operator=(Module&& x) {
		assert(m_is_extern);
		assert(x.m_name == "");
		m_items = ::std::move(x.m_items);
		this->add_attrs( ::std::move(x.m_attrs) );
		return *this;
	}

	bool is_external() const { return m_is_extern; }
	const ::std::string& name() const { return m_name; }
	const ::std::string& filename() const { return m_filename; }
	const ::std::string& base_dir() const { return m_base_dir; }
	const ::std::vector<::std::string>& mod_path() const { return m_mod_path; }

	void set_name(::std::string name) {
		assert(m_name == "");
		m_name = name;
	}

	void set_mod_path(::std::vector<::std::string> mod_path) {
		m_mod_path = ::std::move(mod_path);
	}
	void set_paths(::std::string filename, ::std::string base_dir) {
		m_filename = filename;
		m_base_dir = base_dir;
	}

	ItemList& items() {
		return m_items;
	}
};
class Macro:
	public Item
{
	MacroInv	m_inner;
public:
	Macro(MacroInv mi): m_inner(std::move(mi)) {}
};

class ExternCrate:
	public Item
{
	::std::string	m_name;
	::std::string	m_alias;
public:
	ExternCrate(::std::string name):
		m_name(name),
		m_alias(name)
	{}
	ExternCrate(::std::string name, ::std::string alias):
		m_name(name),
		m_alias(alias)
	{}
};

struct UseEntry_Proto
{
	std::vector< std::string>	m_path;
	::std::string	m_alias;
	std::vector<UseEntry_Proto>	m_inner;

	UseEntry_Proto() {}
	UseEntry_Proto(std::vector<std::string> path) {}
	UseEntry_Proto(std::vector<std::string> path, std::string alias) {}
	UseEntry_Proto(std::vector<std::string> path, std::vector<UseEntry_Proto> inners) {}
};
class UseSet:
	public Item
{
public:
	UseSet(ItemPath path)
	{
	}
	UseSet(ItemPath path, std::string alias)
	{}
	UseSet(ItemPath base_path, std::vector<UseEntry_Proto> entries)
	{}
};

class TypeAlias:
	public Item
{
};

class Enum:
	public Item
{
};
class Struct:
	public Item
{
};
class Union:
	public Item
{
};
class Trait:
	public Item
{
};

class ExternBlock:
	public Item
{
	::std::string	m_abi;
	ItemList	m_items;
public:
	ExternBlock(::std::string abi, ItemList items):
		m_abi( abi ),
		m_items( ::std::move(items) )
	{}
};

class Impl:
	public Item
{
};

class Fn:
	public Item
{
};

