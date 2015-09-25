#pragma once

#include <vector>
#include <memory>
#include <utility>

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

class MetaItem;
typedef ::std::vector<MetaItem>	MetaItems;

class MetaItem
{
	::std::string	m_key;
	::std::string	m_value;
	MetaItems	m_sub_items;
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
};
class Attr
{
public:
	Attr(MetaItems items);
};
typedef ::std::vector<Attr>	AttrList;


class TokenTree;
typedef ::std::vector<TokenTree>	TokenTreeList;

class TokenTree
{
public:
	TokenTree(TokenTreeList tts);
	TokenTree(int tok);
};


class Item
{
public:
	void set_pub();
	void add_attrs(AttrList);
};

class Global:
	public Item
{
};
class Module:
	public Item
{
public:
	Module(::std::string name);
	Module(AttrList attrs, ::std::vector< ::std::unique_ptr<Item> >	items);
	
	void set_name(::std::string name);
};
class Macro:
	public Item
{
public:
	Macro(::std::string name, TokenTree contents);
	Macro(::std::string name, ::std::string ident, TokenTree contents);
};



