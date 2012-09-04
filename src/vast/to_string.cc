#include "vast/to_string.h"

#include <set>
#include <ze/to_string.h>
#include "vast/expression.h"

namespace vast {

std::string to_string(schema::type const& type)
{
  std::string str;
  if (dynamic_cast<schema::bool_type const*>(&type))
  {
    str += "bool";
  }
  else if (dynamic_cast<schema::int_type const*>(&type))
  {
    str += "int";
  }
  else if (dynamic_cast<schema::uint_type const*>(&type))
  {
    str += "count";
  }
  else if (dynamic_cast<schema::double_type const*>(&type))
  {
    str += "double";
  }
  else if (dynamic_cast<schema::time_frame_type const*>(&type))
  {
    str += "interval";
  }
  else if (dynamic_cast<schema::time_point_type const*>(&type))
  {
    str += "time";
  }
  else if (dynamic_cast<schema::string_type const*>(&type))
  {
    str += "string";
  }
  else if (dynamic_cast<schema::regex_type const*>(&type))
  {
    str += "pattern";
  }
  else if (dynamic_cast<schema::address_type const*>(&type))
  {
    str += "addr";
  }
  else if (dynamic_cast<schema::prefix_type const*>(&type))
  {
    str += "subnet";
  }
  else if (dynamic_cast<schema::port_type const*>(&type))
  {
    str += "port";
  }
  else if (auto* e = dynamic_cast<schema::enum_type const*>(&type))
  {
    str += "enum {";
    auto first = e->fields.begin();
    auto last = e->fields.end();
    while (first != last)
    {
        str += *first;
        if (++first != last)
            str += ", ";
    }
    str += '}';
  }
  else if (auto* v = dynamic_cast<schema::vector_type const*>(&type))
  {
    str += "vector of ";
    str += to_string(v->elem_type);
  }
  else if (auto* s = dynamic_cast<schema::set_type const*>(&type))
  {
    str += "set[";
    str += to_string(s->elem_type);
    str += ']';
  }
  else if (auto* t = dynamic_cast<schema::table_type const*>(&type))
  {
    str += "table[";
    str += to_string(t->key_type);
    str += "] of ";
    str += to_string(t->value_type);
  }
  else if (auto* r = dynamic_cast<schema::record_type const*>(&type))
  {
    str += "record {";
    auto first = r->args.begin();
    auto last = r->args.end();
    while (first != last)
    {
        str += to_string(*first);
        if (++first != last)
            str += ", ";
    }
    str += '}';
  }

  return str;
}

std::string to_string(schema::type_info const& ti)
{
  return ti.name != "<anonymous>" ? ti.name : to_string(*ti.type);
}

std::string to_string(schema::event const& e)
{
  std::string str("event ");
  str += e.name + "(";
  auto first = e.args.begin();
  auto last = e.args.end();
  while (first != last)
  {
    str += to_string(*first);
    if (++first != last)
      str += ", ";
  }
  str += ')';
  return str;
}

std::string to_string(schema::argument const& a)
{
  return a.name + ": " + to_string(a.type);
}

std::string to_string(schema const& s)
{
  std::string str;

  // Ignore aliases and built-in types.
  // ...and also all aliases.
  std::set<std::string> aliases;
  static std::set<std::string> builtin{"bool", "int", "count", "double",
                                       "interval", "time", "string", "pattern",
                                       "addr", "subnet", "port"};
  for (auto& t : s.types_)
  {
    if (! (builtin.count(t.name) || aliases.count(t.name)))
    {
      str += "type " + t.name + ": " + to_string(*t.type) + '\n';
      for (auto& a : t.aliases)
      {
        str += "type " + a + ": " + t.name + '\n';
        aliases.insert(a);
      }
    }
  }

  if (! s.events_.empty())
    str += '\n';

  for (auto& e : s.events_)
    str += to_string(e) + '\n';

  return str;
}

class stringifier : public expr::const_visitor
{
public:
  stringifier(std::string& str)
    : str_(str)
  {
  }

  virtual void visit(expr::node const&)
  {
    assert(! "should never happen");
  }

  virtual void visit(expr::timestamp_extractor const&)
  {
    indent();
    str_ += "&time\n";
  }

  virtual void visit(expr::name_extractor const&)
  {
    indent();
    str_ += "&name\n";
  }

  virtual void visit(expr::id_extractor const&)
  {
    indent();
    str_ += "&id\n";
  }

  virtual void visit(expr::offset_extractor const& o)
  {
    indent();
    str_ += '@';
    auto first = o.offsets().begin();
    auto last = o.offsets().end();
    while (first != last)
    {
      str_ += std::to_string(*first);
      if (++first != last)
        str_ += ",";
    }
    str_ += '\n';
  }

  virtual void visit(expr::type_extractor const& e)
  {
    indent();
    str_ += "type(";
    str_ += to_string(e.type());
    str_ += ")\n";
  }

  virtual void visit(expr::conjunction const& conj)
  {
    indent();
    str_ += "&&\n";
    ++depth_;
    for (auto& op : conj.operands())
      op->accept(*this);
    --depth_;
  }

  virtual void visit(expr::disjunction const& disj)
  {
    indent();
    str_ += "||\n";
    ++depth_;
    for (auto& op : disj.operands())
      op->accept(*this);
    --depth_;
  }

  virtual void visit(expr::relational_operator const& rel)
  {
    assert(rel.operands().size() == 2);

    indent();
    switch (rel.type())
    {
      default:
        assert(! "invalid operator type");
        break;
      case expr::match:
        str_ += "~";
        break;
      case expr::not_match:
        str_ += "!~";
        break;
      case expr::in:
        str_ += "in";
        break;
      case expr::not_in:
        str_ += "!in";
        break;
      case expr::equal:
        str_ += "==";
        break;
      case expr::not_equal:
        str_ += "!=";
        break;
      case expr::less:
        str_ += "<";
        break;
      case expr::less_equal:
        str_ += "<=";
        break;
      case expr::greater:
        str_ += ">";
        break;
      case expr::greater_equal:
        str_ += ">=";
        break;
    }
    str_ += '\n';

    ++depth_;
    rel.operands()[0]->accept(*this);
    rel.operands()[1]->accept(*this);
    --depth_;
  }

  virtual void visit(expr::constant const& c)
  {
    indent();
    str_ += to_string(c.result()) + '\n';
  }

private:
  void indent()
  {
    str_ += std::string(depth_ * 2, indent_);
  }

  unsigned depth_ = 0;
  char indent_ = ' ';
  std::string& str_;
};

std::string to_string(expression const& e)
{
  std::string str;
  stringifier visitor(str);
  e.accept(visitor);
  return str;
}

} // namespace vast