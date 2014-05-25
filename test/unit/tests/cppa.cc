#include "framework/unit.h"

#include "cppa/cppa.hpp"
#include "cppa/binary_serializer.hpp"
#include "cppa/binary_deserializer.hpp"
#include "cppa/util/buffer.hpp"

#include "vast/event.h"

using namespace vast;

TEST("libcppa serialization")
{
  event e0{42, "foo", -8.3, record{invalid, now()}};
  e0.id(101);

  cppa::util::buffer buf;
  cppa::binary_serializer bs(&buf);
  cppa::uniform_typeid<event>()->serialize(&e0, &bs);

  event e1;
  cppa::binary_deserializer bd(buf.data(), buf.size());
  cppa::uniform_typeid<event>()->deserialize(&e1, &bd);

  CHECK(e0 == e1);
}