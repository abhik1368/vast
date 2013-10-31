#include "vast/segment.h"

#include "vast/event.h"
#include "vast/logger.h"
#include "vast/serialization.h"
#include "vast/util/make_unique.h"
#include "vast/exception.h"
#include "vast/logger.h"

namespace vast {

// FIXME: Why does the linker complain without these definitions? These are
// redundant to those in the header file.
uint32_t const segment::magic;
uint8_t const segment::version;

bool operator==(segment const& x, segment const& y)
{
  return x.id_ == y.id_;
}

segment::writer::writer(segment* s, size_t max_events_per_chunk)
  : segment_(s),
    chunk_(make_unique<chunk>(segment_->compression_)),
    writer_(make_unique<chunk::writer>(*chunk_)),
    max_events_per_chunk_(max_events_per_chunk)
{
  assert(s != nullptr);
}

segment::writer::~writer()
{
  if (! flush())
    VAST_LOG_WARN("segment writer discarded " <<
                  chunk_->elements() << " events");
}

bool segment::writer::write(event const& e)
{
  if (! (writer_ && store(e)))
    return false;

  if (max_events_per_chunk_ > 0 &&
      chunk_->elements() % max_events_per_chunk_ == 0)
    flush();

  return true;
}

void segment::writer::attach_to(segment* s)
{
  assert(s != nullptr);
  segment_ = s;
}

bool segment::writer::flush()
{
  if (chunk_->empty())
    return true;

  writer_.reset();
  if (! segment_->append(*chunk_))
    return false;

  chunk_ = make_unique<chunk>(segment_->compression_);
  writer_ = make_unique<chunk::writer>(*chunk_);

  return true;
}

size_t segment::writer::bytes() const
{
  return writer_ ? writer_->bytes() : chunk_->uncompressed_bytes();
}

bool segment::writer::store(event const& e)
{
  auto success =
    writer_->write(e.name(), 0) &&
    writer_->write(e.timestamp(), 0);
    writer_->write(static_cast<std::vector<value> const&>(e));

  if (! success)
    VAST_LOG_ERROR("failed to write event entirely to chunk");

  return success;
}


segment::reader::reader(segment const* s)
  : segment_{s},
    id_{segment_->base_}
{
  if (! segment_->chunks_.empty())
    reader_ = make_unique<chunk::reader>(*segment_->chunks_[chunk_idx_]);
}

bool segment::reader::read(event* e)
{
  if (! reader_)
    return false;

  if (reader_->available() == 0)
  {
    if (++chunk_idx_ == segment_->chunks_.size()) // No more events available.
      return false;

    auto& next_chunk = *segment_->chunks_[chunk_idx_];
    reader_ = make_unique<chunk::reader>(next_chunk);
    return read(e);
  }

  return load(e);
}

bool segment::reader::seek(event_id id)
{
  if (! reader_)
    return false;

  if (segment_->base_ == 0)
    return false;

  if (id < segment_->base_ || id >= segment_->base_ + segment_->n_)
    return false;

  chunk const* chk;
  if (id < id_)
  {
    id_ = segment_->base_;
    size_t i = 0;
    while (i < segment_->chunks_.size())
    {
      chk = &segment_->chunks_[i].read();
      if (id_ + chk->elements() > id)
        break;
      id_ += chk->elements();
      ++i;
    }

    reader_ = make_unique<chunk::reader>(*chk);
  }
  else
  {
    auto elements = reader_->available();
    while (chunk_idx_ < segment_->chunks_.size() && id_ + elements < id)
    {
      id_ += elements;
      chk = &segment_->chunks_[++chunk_idx_].read();
      elements = chk->elements();
      if (reader_)
        reader_.reset();
    }

    if (! reader_)
      reader_ = make_unique<chunk::reader>(*chk);
  }

  while (id_ < id)
    if (! read(nullptr))
      return false;

  return true;
}

bool segment::reader::empty() const
{
  return reader_ ? reader_->available() == 0 : true;
}

bool segment::reader::load(event* e)
{
  assert(reader_);

  string name;
  if (! reader_->read(name, 0))
  {
    VAST_LOG_ERROR("failed to read event name from chunk");
    return false;
  }

  time_point t;
  if (! reader_->read(t, 0))
  {
    VAST_LOG_ERROR("failed to read event timestamp from chunk");
    return false;
  }

  std::vector<value> v;
  if (! reader_->read(v))
  {
    VAST_LOG_ERROR("failed to read event arguments from chunk");
    return false;
  }

  if (e != nullptr)
  {
    event r(std::move(v));
    r.name(std::move(name));
    r.timestamp(t);
    if (id_ > 0)
      r.id(id_);
    *e = std::move(r);
  }

  if (id_ > 0)
    ++id_;

  return true;
}


segment::segment(uuid id, size_t max_bytes, io::compression method)
  : id_(id),
    compression_(method),
    max_bytes_(max_bytes)
{
}

uuid const& segment::id() const
{
  return id_;
}

void segment::base(event_id id)
{
  base_ = id;
}

event_id segment::base() const
{
  return base_;
}

bool segment::contains(event_id eid) const
{
  return base_ <= eid && eid < base_ + n_;
}

uint32_t segment::events() const
{
  return n_;
}

uint32_t segment::bytes() const
{
  return occupied_bytes_;
}

size_t segment::max_bytes() const
{
  return max_bytes_;
}

size_t segment::store(std::vector<event> const& v, size_t max_events_per_chunk)
{
  writer w(this, max_events_per_chunk);
  size_t i;
  for (i = 0; i < v.size(); ++i)
    if (! w.write(v[i]))
      break;
  return i;
}

optional<event> segment::load(event_id id) const
{
  reader r(this);
  if (! r.seek(id))
    return {};

  event e;
  if (! r.read(&e))
    return {};

  return {std::move(e)};
}

bool segment::append(chunk& c)
{
  if (max_bytes_ > 0 && bytes() + c.compressed_bytes() > max_bytes_)
    return false;

  n_ += c.elements();
  occupied_bytes_ += c.compressed_bytes();
  chunks_.emplace_back(std::move(c));
  return true;
}

void segment::serialize(serializer& sink) const
{
  sink << magic;
  sink << version;

  sink << id_;
  sink << compression_;
  sink << base_;
  sink << n_;
  sink << occupied_bytes_;
  sink << chunks_;
}

void segment::deserialize(deserializer& source)
{
  uint32_t m;
  source >> m;
  if (m != magic)
    throw error::segment("invalid segment magic");

  uint8_t v;
  source >> v;
  if (v > segment::version)
    throw error::segment("segment version too high");

  source >> id_;
  source >> compression_;
  source >> base_;
  source >> n_;
  source >> occupied_bytes_;
  source >> chunks_;
}

} // namespace vast
