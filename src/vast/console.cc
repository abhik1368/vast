#include "vast/console.h"

#include <cassert>
#include <iomanip>
#include <cppa/cppa.hpp>
#include "vast/event.h"
#include "vast/util/color.h"
#include "vast/util/parse.h"

using namespace cppa;

namespace vast {

namespace {

// Generates a callback function for a mode or command.
struct help_printer
{
  template <typename T>
  auto operator()(T x) const -> util::command_line::callback
  {
    return [=](std::string) -> util::result<bool>
    {
      std::cerr
        << "\noptions for "
        << util::color::cyan << x->name() << util::color::reset << ":\n\n"
        << x->help(4)
        << std::endl;

      return true;
    };
  }
};

help_printer help;

struct cow_event_less_than
{
  bool operator()(cow<event> const& x, cow<event> const& y) const
  {
    return *x < *y;
  }
};

cow_event_less_than cow_event_lt;

} // namespace <anonymous>

console::console(cppa::actor_ptr search, path dir)
  : dir_{std::move(dir)},
    search_{std::move(search)}
{
  if (! exists(dir_))
    if (mkdir(dir_))
      VAST_LOG_ACTOR_ERROR("failed to create console directory: " << dir_);

  auto main
    = cmdline_.mode_add("main", "::: ", to<std::string>(dir_ / "history_main"));

  main->on_unknown_command(help(main));

  main->add("exit", "exit the console")->on(
      [=](std::string) -> util::result<bool>
      {
        quit(exit::stop);
        return {};
      });

  auto set = main->add("set", "adjust console settings");
  set->on(help(set));

  set->add("batch-size", "number of results to display")->on(
      [=](std::string args) -> util::result<bool>
      {
        uint64_t n;
        auto begin = args.begin();
        if (! extract(begin, args.end(), n))
        {
          opts_.batch_size = n;
          return true;
        }
        else
        {
          print(error) << "batch-size requires numeric argument" << std::endl;
          return false;
        }
      });

  auto auto_follow = set->add("auto-follow",
                              "enter interactive control mode after query creation");
  auto_follow->on(
      [=](std::string args) -> util::result<bool>
      {
        match_split(args, ' ')(
            on("T") >> [=](std::string const&)
            {
              opts_.auto_follow = true;
            },
            on("F") >> [=](std::string const&)
            {
              opts_.auto_follow = false;
            },
            others() >> [=]
            {
              print(error) << "need 'T' or 'F' as argument" << std::endl;
              return false;
            });

        return true;
      });

  set->add("show", "display the current settings")->on(
      [=](std::string) -> util::result<bool>
      {
        std::cerr
          << "batch-size = " << opts_.batch_size << '\n'
          << "auto-follow = " << (opts_.auto_follow ? "T" : "F")
          << std::endl;

        return true;
      });

  main->add("ask", "enter query mode")->on(
      [=](std::string) -> util::result<bool>
      {
        cmdline_.mode_push("ask");
        return true;
      });


  main->add("list", "list existing queries")->on(
      [=](std::string) -> util::result<bool>
      {
        for (auto& p : results_)
          std::cout
            << (&p.second == current_result_ ? " * " : "   ")
            << p.second.id() << '\t'
            << p.second.size()
            << std::endl;

        return true;
      });

  main->add("query", "enter a query")->on(
      [=](std::string args) -> util::result<bool>
      {
        if (args.empty())
        {
          print(error) << "missing query UUID" << std::endl;
          return false;
        }

        auto r = to_result(args);
        if (r.first)
        {
          VAST_LOG_ACTOR_DEBUG("enters query " << r.second->id());
          current_result_ = r.second;
          current_query_ = r.first;
          send(self, atom("key"), atom("get"));
          return {};
        }

        return true;
      });

  auto ask = cmdline_.mode_add("ask", "-=> ",
                               to<std::string>(dir_ / "history_query"));

  ask->add("exit", "leave query asking mode")->on(
      [=](std::string) -> util::result<bool>
      {
        cmdline_.mode_pop();
        return true;
      });

  ask->on_unknown_command(
      [=](std::string args) -> util::result<bool>
      {
        if (args.empty())
          return true;

        sync_send(search_, atom("query"), atom("create"), args).then(
            on(atom("EXITED"), arg_match) >> [=](uint32_t reason)
            {
              print(error)
                << "search terminated with exit code " << reason << std::endl;

              send_exit(self, exit::error);
            },
            on_arg_match >> [=](std::string const& failure) // TODO: use error class.
            {
              print(error) << "syntax error: " << failure << std::endl;
              show_prompt();
            },
            on_arg_match >> [=](actor_ptr const& qry, expr::ast const& ast)
            {
              assert(! results_.count(qry));
              assert(qry);
              assert(ast);

              // FIXME: remove
              //cmdline_.append_to_history(args);
              monitor(qry);
              current_query_ = qry;
              current_result_ = &results_.emplace(qry, ast).first->second;
              cmdline_.mode_pop();
              std::cerr
                << "new query " << current_result_->id()
                << " -> " << ast << std::endl;

              if (opts_.auto_follow)
              {
                follow_mode_ = true;
                send(self, atom("key"), atom("get"));
              }
              else
              {
                show_prompt();
              }
            },
            others() >> [=]
            {
              VAST_LOG_ACTOR_ERROR("got unexpected message: " <<
                                   to_string(last_dequeued()));
              show_prompt();
            });

      return {};
    });

  cmdline_.mode_push("main");
}

console::result::result(expr::ast ast)
  : ast_{std::move(ast)}
{
}

void console::result::add(cow<event> e)
{
  auto i = std::lower_bound(events_.begin(), events_.end(), e, cow_event_lt);
  assert(i == events_.end() || cow_event_lt(e, *i));
  events_.insert(i, e);
}

size_t console::result::apply(size_t n, std::function<void(event const&)> f)
{
  size_t i = 0;
  while (i < n && pos_ < events_.size())
  {
    f(*events_[pos_++]);
    ++i;
  }
  return i;
}

size_t console::result::seek_forward(size_t n)
{
  if (pos_ + n >= events_.size())
  {
    auto seeking = static_cast<size_t>(events_.size() - pos_);
    pos_ = events_.size();
    return seeking;
  }
  else
  {
    pos_ += n;
    return n;
  }
}

size_t console::result::seek_backward(size_t n)
{
  if (n > pos_)
  {
    auto old = pos_;
    pos_ = 0;
    return static_cast<size_t>(old);
  }
  else
  {
    pos_ -= n;
    return n;
  }
}

expr::ast const& console::result::ast() const
{
  return ast_;
}

size_t console::result::size() const
{
  return events_.size();
}

void console::act()
{
  chaining(false);

  auto keystroke_monitor = spawn<detached+linked>(
      [=]
      {
        become(
            on(atom("get")) >> [=]
            {
              char c;
              return make_any_tuple(atom("key"), cmdline_.get(c) ? c : 'q');
            });
      });

  become(
      on(atom("DOWN"), arg_match) >> [=](uint32_t)
      {
        VAST_LOG_ACTOR_ERROR("got DOWN from query @" << last_sender()->id());
        // TODO: seal corresponding result
      },
      on(atom("done")) >> [=]
      {
        VAST_LOG_ACTOR_DEBUG("got done from query @" << last_sender()->id());
        show_prompt();
        // TODO: seal corresponding query.
      },
      on(atom("prompt")) >> [=]
      {
        // An empty result means that we should not go back to the prompt.
        auto result = cmdline_.process();
        if (result.engaged())
        {
          // TODO: give visual clue if result failed.

          show_prompt();
        }
        else if (result.failed())
        {
          print(error) << result.failure() << std::endl;
          show_prompt();
        }
      },
      on_arg_match >> [=](event const&)
      {
        auto i = results_.find(last_sender());
        assert(i != results_.end());
        auto r = &i->second;
        cow<event> ce = *tuple_cast<event>(last_dequeued());
        r->add(ce);
        if (r == current_result_ && follow_mode_)
          std::cout << *ce << std::endl;
      },
      on(atom("key"), atom("get")) >> [=]
      {
        send(keystroke_monitor, atom("get"));
      },
      on(atom("key"), arg_match) >> [=](char key)
      {
        switch (key)
        {
          default:
            {
              print(error)
                << "invalid key: '" << key << "', press '?' for help"
                << std::endl;
            }
            break;
          case '?':
            {
              std::cerr
                << "interactive query control mode:\n\n"
                << "    <space>  display the next batch of available results\n"
                << "       e     ask query for more results\n"
                << "       f     toggle follow mode\n"
                << "       j     seek one batch forward\n"
                << "       k     seek one batch backword\n"
                << "       ?     display this help\n"
                << "       q     leave query control mode\n"
                << std::endl;
            }
            break;
          case ' ':
            {
              auto n = current_result_->apply(
                  opts_.batch_size,
                  [](event const& e) { std::cout << e << std::endl; });
              if (n == 0)
                print(query) << "reached end of results" << std::endl;
            }
            break;
          case 'e':
            {
              send(current_query_, atom("extract"));
              print(query) << "asks for more results" << std::endl;
            }
            break;
          case 'f':
            {
              follow_mode_ = ! follow_mode_;
              print(query)
                << "toggled follow-mode to " << (follow_mode_ ? "on" : "off")
                << std::endl;
            }
            break;
          case 'j':
            {
              auto n = current_result_->seek_forward(opts_.batch_size);
              print(query) << "seeked +" << n << " events" << std::endl;
            }
            break;
          case 'k':
            {
              auto n = current_result_->seek_backward(opts_.batch_size);
              print(query) << "seeked -" << n << " events" << std::endl;
            }
            break;
          case '':
          case '\n':
          case 'q':
            {
              follow_mode_ = false;
              show_prompt();
            }
            return;
        }
        send(keystroke_monitor, atom("get"));
      },
      others() >> [=]
      {
        VAST_LOG_ACTOR_ERROR("got unexpected message from @" <<
                             last_sender()->id() << ": " <<
                             to_string(last_dequeued()));
      });
}

char const* console::description() const
{
  return "console";
}

std::ostream& console::print(print_mode mode) const
{
  switch (mode)
  {
    default:
      std::cerr << util::color::red << "[???] ";
      break;
    case error:
      std::cerr << util::color::red << "[error] ";
      break;
    case query:
      std::cerr
        << util::color::cyan << "[query " << current_result_->id() << "] ";
      break;
  }

  std::cerr << util::color::reset;

  return std::cerr;
}

void console::show_prompt(size_t ms)
{
  // The delay allows for logging messages to trickle through first
  // before we print the prompt.
  delayed_send(self, std::chrono::milliseconds(ms), atom("prompt"));
}

std::pair<actor_ptr, console::result*>
console::to_result(std::string const& str)
{
  std::vector<result*> matches;
  actor_ptr qry = nullptr;
  for (auto& p : results_)
  {
    auto candidate = to<std::string>(p.second.id());
    auto i = std::mismatch(str.begin(), str.end(), candidate.begin());
    if (i.first == str.end())
    {
      qry = p.first;
      matches.push_back(&p.second);
    }
  }
  if (matches.empty())
    print(error) << "no such query: " << str << std::endl;
  else if (matches.size() > 1)
    print(error) << "ambiguous query: " << str << std::endl;
  else
    return {qry, matches[0]};
  return {nullptr, nullptr};
}

} // namespace vast
