
#include "../../src/include/tagged_union.hpp"

TAGGED_UNION_EX(TestTu, (), Foo, (
	(Foo, struct {
		}),
	(Bar, struct {
		}),
	(Baz, unsigned int)
	),
	(), (), ()
	);
TAGGED_UNION(TestTu2, Foo,
	(Foo, struct {
		}),
	(Bar, struct {
		}),
	(Baz, unsigned int)
	);

int main()
{
	TestTu	tmp;

	tmp = TestTu(123);

	TestTu	tmp2 = TestTu::make_Bar({});

	TU_MATCH_HDRA( (tmp), {)
	TU_ARMA(Foo, e) {
		}
	TU_ARMA(Bar, e) {
		}
	TU_ARMA(Baz, e) {
		}
	}

	TU_MATCH_HDRA( (tmp,tmp2), { )
	TU_ARMA(Foo, e,e2) {
		}
	TU_ARMA(Bar, e,e2) {
		}
	TU_ARMA(Baz, e,e2) {
		}
	}

#if 1
    switch(tmp.tag())
    {
    TU_ARM(tmp, Baz, e) {
        } break;
    TU_ARM(tmp, Bar, e) {
        } break;
    }
#endif
}
