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
template<typename T>
::std::unique_ptr<T> box_raw(T *ptr) {
	return ::std::unique_ptr<T>(ptr);
}
template<typename T>
::std::unique_ptr<T> box(T&& ptr) {
	return ::std::unique_ptr<T>(new T(::std::move(ptr)));
}


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

class MetaItem;
typedef ::std::vector<MetaItem>	MetaItems;

class MetaItem
{
	::std::string	m_key;
	::std::string	m_value;
	MetaItems	m_sub_items;
	// TODO: How to represent `#[attr()]` (i.e. empty paren set) as distinct from `#[attr=""]` and `#[attr]`
public:
	MetaItem(::std::string key):
		m_key(key)
	{}
	MetaItem(::std::string key, ::std::string value):
		m_key(key),
		m_value(value)
	{}
	MetaItem(::std::string key, MetaItems sub_items):
		m_key(key),
		m_sub_items(sub_items)
	{}

	const ::std::string& key() const { return m_key; }
	const ::std::string& string() const { assert(m_sub_items.size() == 0); return m_value; }
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

	void set_pub() {
		m_is_pub = true;
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
	::std::string	m_name;
	::std::string	m_ident;
	TokenTree	m_args;
public:
	Macro(::std::string name, TokenTree contents):
		m_name(name),
		m_ident(),
		m_args(contents)
	{}
	Macro(::std::string name, ::std::string ident, TokenTree contents):
		m_name(name),
		m_ident(ident),
		m_args(contents)
	{}
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

class UseItem
{
	::std::string	m_name;
public:
	struct TagSelf {};
	UseItem(TagSelf):
		m_name()
	{}
	UseItem(::std::string name):
		m_name(name)
	{}
};
class UseItems
{
	::std::string	m_alias;
	::std::vector<UseItem>	m_items;
public:
	UseItems():
		m_alias()
	{}
	struct TagWildcard {};
	UseItems(TagWildcard):
		m_alias("*")
	{}
	struct TagRename {};
	UseItems(TagRename, ::std::string name):
		m_alias(name)
	{}
	UseItems(::std::vector<UseItem> items):
		m_alias(),
		m_items(items)
	{}
};
class UseSet:
	public Item
{
public:
	UseSet()
	{}
	UseSet(Path base, UseItems items)
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

