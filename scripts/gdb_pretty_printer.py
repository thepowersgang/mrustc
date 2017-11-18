import gdb

class EnumPrinter(object):
    def __init__(self, val):
        # TODO: Support NonZero optimised enums
        self.tag = int(val['TAG'])
        self.var_data = val['DATA']['var_%i' % (self.tag,)]
    def to_string(self):
        return "var %i" % (self.tag,)
    def children(self):
        for f in self.var_data.type.fields():
            yield (f.name, self.var_data[f.name],)
        return

def register():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("mrustc")
    pp.add_printer('enum', '^e__', EnumPrinter)
    return pp
gdb.printing.register_pretty_printer( gdb.current_objfile(), register(), replace=True )
