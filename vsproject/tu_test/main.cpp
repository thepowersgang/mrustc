
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

	TU_MATCHA( (tmp), (e),
	(Foo,
		),
	(Bar,
		),
	(Baz,
		)
	)

	TU_MATCHA((tmp,tmp2), (e,e2),
	(Foo,
		),
	(Bar,
		),
	(Baz,
		)
	)
}
