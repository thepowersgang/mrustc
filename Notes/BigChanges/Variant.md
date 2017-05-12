
(the below is from @ubsan)

```
// one can define their own variant, but I'm lazy
#include <variant>
#include <type_traits>
#include <iostream>

template <typename F, typename T, typename Void = void>
struct return_type {};

template <typename F, typename T>
struct return_type<T, F, std::void_t<
  decltype(std::declval<F>()(std::declval<T>()))
>> {
  using type = decltype(std::declval<F>()(std::declval<T>()));
};

template <typename F, typename T>
using return_type_t = typename return_type<T, F>::type;

// not sure what to call this
template <typename F, typename... Ts>
struct common_return_type {
  using type = std::common_type_t<return_type_t<F, Ts>...>;
};

template <typename F, typename... Ts>
using common_return_type_t = typename common_return_type<F, Ts...>::type;

template <typename F, typename... Ts>
auto match(
  std::variant<Ts...>& variant, F&& functor
) -> common_return_type_t<F, Ts...> {
  // you could also use static_assert to make it SFINAE-unfriendly
  return std::visit(functor, variant);
}

template <typename... Fs>
struct overloaded : Fs... {
  using Fs::operator()...;
  overloaded(Fs&&... fs) : Fs(std::forward<Fs>(fs))... { }
};

int main() {
  auto var = std::variant<int, std::string>(0);
  std::cout << match(var, overloaded(
    [](int i) { return i; },
    [](std::string s) { return 0; }
  ));
}
```



ALTERNATIVE
===========

Could just update TU to have:
``
stmt.if_let(
stmt.match_def(
stmt.match(
    [](::MIR::LValue::Data_Assign& se) {
    },
    [](::MIR::LValue::Data_Drop& se) {
    }
    );
```
