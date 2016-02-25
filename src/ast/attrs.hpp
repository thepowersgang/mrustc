#ifndef _AST_ATTRS_HPP_
#define _AST_ATTRS_HPP_


namespace AST {

//
class MetaItem;

class MetaItems:
    public Serialisable
{
public:
    ::std::vector<MetaItem> m_items;
    
    MetaItems() {}
    MetaItems(::std::vector<MetaItem> items):
        m_items(items)
    {
    }
    
    void push_back(MetaItem i);
    
    MetaItem* get(const char *name);
    bool has(const char *name) {
        return get(name) != 0;
    }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const MetaItems& x) {
        return os << "[" << x.m_items << "]";
    }
    
    SERIALISABLE_PROTOTYPES();
};

class MetaItem:
    public Serialisable
{
    ::std::string   m_name;
    MetaItems   m_sub_items;
    ::std::string   m_str_val;
public:
    MetaItem() {}
    MetaItem(::std::string name):
        m_name(name)
    {
    }
    MetaItem(::std::string name, ::std::string str_val):
        m_name(name),
        m_str_val(str_val)
    {
    }
    MetaItem(::std::string name, ::std::vector<MetaItem> items):
        m_name(name),
        m_sub_items(items)
    {
    }
    
    void mark_used() {}
    const ::std::string& name() const { return m_name; }
    const ::std::string& string() const { return m_str_val; }
    bool has_sub_items() const { return m_sub_items.m_items.size() > 0; }
    const MetaItems& items() const { return m_sub_items; }
    MetaItems& items() { return m_sub_items; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const MetaItem& x) {
        os << x.m_name;
        if(x.m_sub_items.m_items.size())
            os << "(" << x.m_sub_items.m_items << ")";
        else
            os << "=\"" << x.m_str_val << "\"";
        return os;
    }
    
    SERIALISABLE_PROTOTYPES();
};

}   // namespace AST

#endif

