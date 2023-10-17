/*

Copyright (c) 2023, Arvid Norberg
All rights reserved.

You may use, distribute and modify this code under the terms of the BSD license,
see LICENSE file.
*/

#ifndef TORRENT_DISK_CACHE
#define TORRENT_DISK_CACHE

#include <unordered_map>
#include <mutex>

#include "libtorrent/storage_defs.hpp"
#include "libtorrent/aux_/scope_end.hpp"
#include "libtorrent/aux_/alloca.hpp"
#include "libtorrent/aux_/invariant_check.hpp"

#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/functional/hash.hpp>

#define BOOST_BIND_NO_PLACEHOLDERS

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>

#include <boost/functional/hash.hpp>

#include "libtorrent/aux_/disable_warnings_pop.hpp"


namespace libtorrent {
namespace aux {

namespace mi = boost::multi_index;

// uniquely identifies a torrent and piece
struct piece_location
{
	piece_location(storage_index_t const t, piece_index_t const p)
		: torrent(t), piece(p) {}
	storage_index_t torrent;
	piece_index_t piece;
	bool operator==(piece_location const& rhs) const
	{
		return std::tie(torrent, piece)
			== std::tie(rhs.torrent, rhs.piece);
	}

	bool operator<(piece_location const& rhs) const
	{
		return std::tie(torrent, piece)
			< std::tie(rhs.torrent, rhs.piece);
	}
};

inline size_t hash_value(piece_location const& l)
{
	std::size_t ret = 0;
	boost::hash_combine(ret, std::hash<storage_index_t>{}(l.torrent));
	boost::hash_combine(ret, std::hash<piece_index_t>{}(l.piece));
	return ret;
}

struct cached_block_entry
{
	span<char const> buf() const {
		if (buf_holder)
			return {buf_holder.data(), buf_holder.size()};

		if (write_job != nullptr)
		{
			TORRENT_ASSERT(write_job->get_type() == aux::job_action_t::write);
			auto const& job = std::get<job::write>(write_job->action);
			return {job.buf.data(), job.buffer_size};
		}
		return {nullptr, 0};
	}
	// once the write job has been executed, and we've flushed the buffer, we
	// move it into buf_holder, to keep the buffer alive until any hash job has
	// completed as well. The underlying data can be accessed through buf, but
	// the owner moves from the pread_disk_job object to this buf_holder.
	// TODO: save space by just storing the buffer pointer here. The
	// cached_piece_entry could hold the pointer to the buffer pool to be able
	// to free these on destruction
	// we would still need to save the *size* of the block, to support the
	// shorter last block of a torrent
	disk_buffer_holder buf_holder;
	pread_disk_job* write_job = nullptr;

	bool flushed_to_disk = false;

	// TODO: only allocate this field for v2 torrents
	sha256_hash block_hash;
};

struct cached_piece_entry
{
	cached_piece_entry(piece_location const& loc, int const num_blocks)
		: piece(loc)
		, blocks_in_piece(num_blocks)
		, blocks(std::make_unique<cached_block_entry[]>(num_blocks))
		, ph(hasher())
	{}

	span<cached_block_entry> get_blocks() const
	{
		return {blocks.get(), blocks_in_piece};
	}

	piece_location piece;

	// this is set to true when the piece has been populated with all blocks
	// it will make it prioritized for flushing to disk
	bool ready_to_flush = false;

	// when this is true, there is a thread currently hashing blocks and
	// updating the hash context in "ph".
	bool hashing = false;

	// when a thread is writing this piece to disk, this is true. Only one
	// thread at a time should be flushing a piece to disk.
	bool flushing = false;

	// this is set to true if the piece hash has been computed and returned
	// to the bittorrent engine.
	bool piece_hash_returned = false;

	// this indicates that this piece belongs to a v2 torrent, and it has the
	// block_hash member of cached_block_entry and we need to compute the block
	// hashes as well
	bool v1_hashes = false;
	bool v2_hashes = false;

	// TODO: bool v1_hashes = false;

	int blocks_in_piece = 0;

	// the number of blocks that have been hashed so far. Specifically for the
	// v1 SHA1 hash of the piece, so all blocks are contiguous starting at block
	// 0.
	int hasher_cursor = 0;

	// the number of contiguous blocks, starting at 0, that have been flushed to
	// disk so far. This is used to determine how many blocks are left to flush
	// from this piece without requiring read-back to hash them, by substracting
	// flushed_cursor from hasher_cursor.
	int flushed_cursor = 0;

	// returns the number of blocks in this piece that have been hashed and
	// ready to be flushed without requiring reading them back in the future.
	int cheap_to_flush() const
	{
		return int(hasher_cursor) - int(flushed_cursor);
	}

	std::unique_ptr<cached_block_entry[]> blocks;

	hasher ph;

	// if there is a hash_job set on this piece, whenever we complete hashing
	// the last block, we should post this
	pread_disk_job* hash_job = nullptr;

	// if the piece has been requested to be cleared, but it was locked
	// (flushing) at the time. We hang this job here to complete it once the
	// thread currently flushing is done with it
	pread_disk_job* clear_piece = nullptr;
};

struct compare_storage
{
	bool operator()(piece_location const& lhs, storage_index_t const rhs) const
	{
		return lhs.torrent < rhs;
	}

	bool operator()(storage_index_t const lhs, piece_location const& rhs) const
	{
		return lhs < rhs.torrent;
	}
};

static bool have_buffers(span<const cached_block_entry> blocks)
{
	for (auto const& b : blocks)
		if (b.buf().data() == nullptr) return false;
	return true;
}

static bool compute_ready_to_flush(span<const cached_block_entry> blocks)
{
	for (auto const& b : blocks)
	{
		if (!b.write_job && !b.flushed_to_disk) return false;
	}
	return true;
}

static int compute_flushed_cursor(span<const cached_block_entry> blocks)
{
	int ret = 0;
	for (auto const& b : blocks)
	{
		if (!b.flushed_to_disk) return ret;
		++ret;
	}
	return ret;
}

static int count_jobs(span<const cached_block_entry> blocks)
{
	return std::count_if(blocks.begin(), blocks.end()
		, [](cached_block_entry const& b) { return b.write_job; });
}

struct disk_cache
{
	using piece_container = mi::multi_index_container<
		cached_piece_entry,
		mi::indexed_by<
		// look up ranges of pieces by (torrent, piece-index)
		mi::ordered_unique<mi::member<cached_piece_entry, piece_location, &cached_piece_entry::piece>>,
		// ordered by the number of contiguous blocks we can flush without
		// read-back. large numbers are ordered first
		mi::ordered_non_unique<mi::const_mem_fun<cached_piece_entry, int, &cached_piece_entry::cheap_to_flush>, std::greater<void>>,
		// ordered by whether the piece is ready to be flushed or not
		// true is ordered before false
		mi::ordered_non_unique<mi::member<cached_piece_entry, bool, &cached_piece_entry::ready_to_flush>, std::greater<void>>,
		// hash-table lookup of individual pieces. faster than index 0
		mi::hashed_unique<mi::member<cached_piece_entry, piece_location, &cached_piece_entry::piece>>
		>
	>;

	template <typename Fun>
	bool get(piece_location const loc, int const block_idx, Fun f) const
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto i = view.find(loc);
		if (i == view.end()) return false;

		if (i->blocks[block_idx].buf().data())
		{
			// TODO: it would be nice if this could be called without holding
			// the mutex. It would require being able to lock the piece
			f(i->blocks[block_idx].buf());
			return true;
		}
		return false;
	}

	template <typename Fun>
	sha256_hash hash2(piece_location const loc, int const block_idx, Fun f) const
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto i = view.find(loc);
		if (i != view.end())
		{
			if (i->hashing)
			{
				// TODO: it would probably be more efficient to wait here.
				// #error we should hang the hash job onto the piece. If there is a
				// job already, form a queue
				l.unlock();
				return f();
			}
			auto const& cbe = i->blocks[block_idx];
			// There's nothing stopping the hash threads from hashing the blocks in
			// parallel. This should not depend on the hasher_cursor. That's a v1
			// concept
			if (i->hasher_cursor > block_idx)
				return cbe.block_hash;
			if (cbe.buf().data())
			{
				hasher256 h;
				h.update(cbe.buf());
				return h.final();
			}
		}
		l.unlock();
		return f();
	}

	// returns false if the piece is not in the cache
	template <typename Fun>
	bool hash_piece(piece_location const loc, Fun f)
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto piece_iter = view.find(loc);
		if (piece_iter == view.end()) return false;

		TORRENT_ALLOCA(blocks, char const*, piece_iter->blocks_in_piece);
		TORRENT_ALLOCA(v2_hashes, sha256_hash, piece_iter->blocks_in_piece);

		for (int i = 0; i < piece_iter->blocks_in_piece; ++i)
		{
			blocks[i] = piece_iter->blocks[i].buf().data();
			v2_hashes[i] = piece_iter->blocks[i].block_hash;
		}

		view.modify(piece_iter, [](cached_piece_entry& e) { e.hashing = true; });
		int const hasher_cursor = piece_iter->hasher_cursor;
		l.unlock();

		auto se = scope_end([&] {
			l.lock();
			view.modify(piece_iter, [&](cached_piece_entry& e) {
				e.hashing = false;
			});
		});
		f(const_cast<hasher&>(piece_iter->ph), hasher_cursor, blocks, v2_hashes);
		return true;
	}

	// If the specified piece exists in the cache, and it's unlocked, clear all
	// write jobs (return them in "aborted"). Returns true if the clear_piece
	// job should be posted as complete. Returns false if the piece is locked by
	// another thread, and the clear_piece job has been queued to be issued once
	// the piece is unlocked.
	bool try_clear_piece(piece_location const loc, pread_disk_job* j, jobqueue_t& aborted)
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto i = view.find(loc);
		if (i == view.end()) return true;
		if (i->flushing)
		{
			// postpone the clearing until we're done flushing
			view.modify(i, [&](cached_piece_entry& e) { e.clear_piece = j; });
			return false;
		}

		// we clear a piece after it fails the hash check. It doesn't make sense
		// to be hashing still
		TORRENT_ASSERT(!i->hashing);
		if (i->hashing)
		{
			// postpone the clearing until we're done flushing
			view.modify(i, [&](cached_piece_entry& e) { e.clear_piece = j; });
			return false;
		}

		view.modify(i, [&](cached_piece_entry& e) {
			clear_piece_impl(e, aborted);
		});
		return true;
	}

	template <typename Fun>
	int get2(piece_location const loc, int const block_idx, Fun f) const
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto i = view.find(loc);
		if (i == view.end()) return 0;

		char const* buf1 = i->blocks[block_idx].buf().data();
		char const* buf2 = i->blocks[block_idx + 1].buf().data();

		if (buf1 == nullptr && buf2 == nullptr)
			return 0;

		return f(buf1, buf2);
	}

	// returns true if this piece needs to have its hasher kicked
	bool insert(piece_location const loc
		, int const block_idx
		, pread_disk_job* write_job)
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto i = view.find(loc);
		if (i == view.end())
		{
//#error this computation is not right for v2 torrents. it will make v2 hashes computed incorrectly
//#error we don't know what the block size actually is here. If the piece size is less than 16 kiB, this computation is incorrect
			pread_storage* storage = write_job->storage.get();
			int const blocks_in_piece = (storage->files().piece_size(loc.piece) + default_block_size - 1) / default_block_size;
			cached_piece_entry pe(loc, blocks_in_piece);
			pe.v1_hashes = storage->v1();
			pe.v2_hashes = storage->v2();
			i = m_pieces.insert(std::move(pe)).first;
		}

		cached_block_entry& blk = i->blocks[block_idx];
		TORRENT_ASSERT(!blk.buf_holder);
		TORRENT_ASSERT(blk.write_job == nullptr);
		TORRENT_ASSERT(blk.flushed_to_disk == false);
		TORRENT_ASSERT(block_idx >= i->flushed_cursor);
		TORRENT_ASSERT(block_idx >= i->hasher_cursor);

		TORRENT_ASSERT(write_job->get_type() == aux::job_action_t::write);
		blk.write_job = write_job;
		++m_blocks;

		bool const ready_to_flush = compute_ready_to_flush(i->get_blocks());
		if (ready_to_flush != i->ready_to_flush)
		{
			view.modify(i, [&](cached_piece_entry& e) {
				e.ready_to_flush = ready_to_flush;
			});
		}

		return block_idx == 0 || ready_to_flush;
	}

	enum hash_result: std::uint8_t
	{
		job_completed,
		job_queued,
		post_job,
	};

	// this call can have 3 outcomes:
	// 1. the job is immediately satisfied and should be posted to the
	//    completion queue
	// 2. The piece is in the cache and currently hashing, but it's not done
	//    yet. We hang the hash job on the piece itself so the hashing thread
	//    can complete it when hashing finishes
	// 3. The piece is not in the cache and should be posted to the disk thread
	//    to read back the bytes.
	hash_result try_hash_piece(piece_location const loc, pread_disk_job* hash_job)
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto i = view.find(loc);
		if (i == view.end()) return hash_result::post_job;

		// we should only ask for the hash once
		TORRENT_ASSERT(!i->piece_hash_returned);

		if (!i->hashing && i->hasher_cursor == i->blocks_in_piece)
		{
			view.modify(i, [&](cached_piece_entry& e) {
				e.piece_hash_returned = true;

				job::hash& job = std::get<aux::job::hash>(hash_job->action);
				job.piece_hash = e.ph.final();
			});
			return hash_result::job_completed;
		}

		if (i->hashing
			&& i->hasher_cursor < i->blocks_in_piece
			&& have_buffers(i->get_blocks().subspan(i->hasher_cursor))
			)
		{
			// We're not done hashing yet, let the hashing thread post the
			// completion once it's done

			// We don't expect to ever have simultaneous async_hash() requests
			// for the same piece
			TORRENT_ASSERT(i->hash_job == nullptr);
			view.modify(i, [&](cached_piece_entry& e) { e.hash_job = hash_job; });
			return hash_result::job_queued;
		}

		return hash_result::post_job;
	}

	// this should be called from a hasher thread
	void kick_hasher(piece_location const& loc, jobqueue_t& completed_jobs)
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& view = m_pieces.template get<0>();
		auto piece_iter = view.find(loc);
		if (piece_iter == view.end())
			return;

		// some other thread beat us to it
		if (piece_iter->hashing)
			return;

		TORRENT_ALLOCA(blocks_storage, span<char const>, piece_iter->blocks_in_piece);
		int cursor = piece_iter->hasher_cursor;
keep_going:
		int i = 0;
		int end = cursor;
		while (end < piece_iter->blocks_in_piece && piece_iter->blocks[end].buf().data())
		{
			blocks_storage[i] = piece_iter->blocks[end].buf();
			++i;
			++end;
		}
		auto blocks = blocks_storage.first(i);

		hasher& ctx = const_cast<hasher&>(piece_iter->ph);

		view.modify(piece_iter, [](cached_piece_entry& e) { e.hashing = true; });

		bool const need_v1 = piece_iter->v1_hashes;
		bool const need_v2 = piece_iter->v2_hashes;

		l.unlock();

		for (auto& buf: blocks)
		{
			cached_block_entry& cbe = piece_iter->blocks[cursor];

			if (need_v1)
				ctx.update(buf);

			if (need_v2)
				cbe.block_hash = hasher256(buf).final();

			++cursor;
		}

		l.lock();
		for (auto& cbe : piece_iter->get_blocks().subspan(piece_iter->hasher_cursor, i))
		{
			// TODO: free these in bulk, acquiring the mutex just once
			// free them after releasing the mutex, l
			if (cbe.buf_holder)
				cbe.buf_holder.reset();
		}

		view.modify(piece_iter, [&](cached_piece_entry& e) {
			e.hasher_cursor = cursor;
			e.hashing = false;
		});

		if (cursor != piece_iter->blocks_in_piece)
		{
			// if some other thread added the next block, keep going
			if (piece_iter->blocks[cursor].buf().data())
				goto keep_going;
		}

		if (!piece_iter->hash_job) return;

		// there's a hash job hung on this piece, post it now
		pread_disk_job* j = nullptr;
		span<cached_block_entry> const cached_blocks = piece_iter->get_blocks();
		view.modify(piece_iter, [&cached_blocks, &j](cached_piece_entry& e) {
			j = std::exchange(e.hash_job, nullptr);
			e.ready_to_flush = compute_ready_to_flush(cached_blocks);
		});
		// we've hashed all blocks, and there's a hash job associated with
		// this piece, post it.
		sha1_hash const piece_hash = ctx.final();

		job::hash& job = std::get<job::hash>(j->action);
		job.piece_hash = piece_hash;
		if (!job.block_hashes.empty())
		{
			TORRENT_ASSERT(need_v2);
			int const to_copy = std::min(
				piece_iter->blocks_in_piece,
				int(job.block_hashes.size()));
			for (int i = 0; i < to_copy; ++i)
				job.block_hashes[i] = piece_iter->blocks[i].block_hash;
		}
		completed_jobs.push_back(j);
	}

	// this should be called by a disk thread
	// the callback should return the number of blocks it successfully flushed
	// to disk
	void flush_to_disk(std::function<int(bitfield&, span<cached_block_entry const>, int)> f
		, int const target_blocks
		, std::function<void(jobqueue_t, pread_disk_job*)> clear_piece_fun)
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		// TODO: refactor this to avoid so much duplicated code

		bitfield flushed_blocks;

		// first we look for pieces that are ready to be flushed and should be
		// updating
		auto& view = m_pieces.template get<2>();
		for (auto piece_iter = view.begin(); piece_iter != view.end();)
		{
#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
			INVARIANT_CHECK;
#endif

			// We want to flush all pieces that are ready to flush regardless of
			// the flush target. There's not much value in keeping them in RAM
			// when we've completely downloaded the piece and hashed it
			// so, we don't check flush target in this loop

			if (piece_iter->flushing)
			{
				++piece_iter;
				continue;
			}

			if (!piece_iter->ready_to_flush)
				break;

			view.modify(piece_iter, [](cached_piece_entry& e) { TORRENT_ASSERT(e.flushing == false); e.flushing = true; });
			int const num_blocks = piece_iter->blocks_in_piece;
			m_flushing_blocks += num_blocks;

			int const hash_cursor = piece_iter->hasher_cursor;
			TORRENT_ASSERT(num_blocks >= 0);
			span<cached_block_entry> const blocks = piece_iter->get_blocks();

			// we have to release the lock while flushing, but since we set the
			// "flushing" member to true, this piece is pinned to the cache
			l.unlock();

			TORRENT_ASSERT(piece_iter->ready_to_flush);
			TORRENT_ASSERT(num_blocks > 0);

			int count = 0;
			{
				auto se = scope_end([&] {
					l.lock();
					view.modify(piece_iter, [](cached_piece_entry& e) {
						e.flushing = false;
						TORRENT_ASSERT(e.ready_to_flush);
					});
					TORRENT_ASSERT(m_flushing_blocks >= num_blocks);
					m_flushing_blocks -= num_blocks;
				});
				flushed_blocks.resize(blocks.size());
				flushed_blocks.clear_all();
				count = f(flushed_blocks, blocks, hash_cursor);
			}

			// now that we hold the mutex again, we can update the entries for
			// all the blocks that were flushed
			for (int i = 0; i < blocks.size(); ++i)
			{
				if (!flushed_blocks.get_bit(i)) continue;
				cached_block_entry& blk = blocks[i];

				auto* j = blk.write_job;
				TORRENT_ASSERT(j);
				TORRENT_ASSERT(j->get_type() == aux::job_action_t::write);
				blk.buf_holder = std::move(std::get<aux::job::write>(j->action).buf);
				blk.flushed_to_disk = true;
				TORRENT_ASSERT(blk.buf_holder);
				// TODO: free these in bulk at the end, or something
				if (i < hash_cursor)
					blk.buf_holder.reset();

				blk.write_job = nullptr;
			}
			view.modify(piece_iter, [&blocks](cached_piece_entry& e) {
				e.flushed_cursor = compute_flushed_cursor(blocks);
				e.ready_to_flush = compute_ready_to_flush(blocks);
			});

			TORRENT_ASSERT(m_blocks >= count);
			m_blocks -= count;
			if (piece_iter->clear_piece)
			{
				jobqueue_t aborted;
				pread_disk_job* clear_piece = nullptr;
				view.modify(piece_iter, [&](cached_piece_entry& e) {
					clear_piece_impl(e, aborted);
					clear_piece = std::exchange(e.clear_piece, nullptr);
				});
				clear_piece_fun(std::move(aborted), clear_piece);
			}
			if (piece_iter->piece_hash_returned)
			{
				TORRENT_ASSERT(!piece_iter->flushing);
				TORRENT_ASSERT(!piece_iter->hashing);
				piece_iter = view.erase(piece_iter);
			}
			else
				++piece_iter;

			if (count < num_blocks)
				return;
		}

		// if we get here, we have to "force flush" some blocks even though we
		// don't have all the blocks yet. Start by flushing pieces that have the
		// most contiguous blocks to flush:
		auto& view2 = m_pieces.template get<1>();
		for (auto piece_iter = view2.begin(); piece_iter != view2.end(); ++piece_iter)
		{
#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
			INVARIANT_CHECK;
#endif

			// We avoid flushing if other threads have already initiated sufficient
			// amount of flushing
			if (m_blocks - m_flushing_blocks <= target_blocks)
				return;

			if (piece_iter->flushing)
				continue;

			int const num_blocks = piece_iter->hasher_cursor - piece_iter->flushed_cursor;

			// the pieces are ordered by the number of blocks that are cheap to
			// flush (i.e. won't require read-back later)
			// if we encounter a 0, all the remaining ones will also be zero
			if (num_blocks <= 0) break;

			TORRENT_ASSERT(num_blocks >= 0);
			span<cached_block_entry> const blocks = piece_iter->get_blocks().subspan(piece_iter->flushed_cursor);
			if (num_blocks == 0) continue;

			view2.modify(piece_iter, [](cached_piece_entry& e) { TORRENT_ASSERT(!e.flushing); e.flushing = true; });

			m_flushing_blocks += num_blocks;

			int const hash_cursor = piece_iter->hasher_cursor;

			// we have to release the lock while flushing, but since we set the
			// "flushing" member to true, this piece is pinned to the cache
			l.unlock();

			int count = 0;
			{
				auto se = scope_end([&] {
					l.lock();
					view2.modify(piece_iter, [&](cached_piece_entry& e) {
						TORRENT_ASSERT(e.flushing); 
						e.flushing = false;
					});
					TORRENT_ASSERT(m_flushing_blocks >= num_blocks);
					m_flushing_blocks -= num_blocks;
				});
				flushed_blocks.resize(blocks.size());
				flushed_blocks.clear_all();
				count = f(flushed_blocks, blocks
					, piece_iter->hasher_cursor - piece_iter->flushed_cursor);
			}

			// now that we hold the mutex again, we can update the entries for
			// all the blocks that were flushed
			for (int i = 0; i < blocks.size(); ++i)
			{
				if (!flushed_blocks.get_bit(i)) continue;
				cached_block_entry& blk = blocks[i];

				auto* j = blk.write_job;
				TORRENT_ASSERT(j);
				TORRENT_ASSERT(j->get_type() == aux::job_action_t::write);
				blk.buf_holder = std::move(std::get<aux::job::write>(j->action).buf);
				blk.flushed_to_disk = true;
				TORRENT_ASSERT(blk.buf_holder);
				// TODO: free these in bulk at the end, or something
				if (i < hash_cursor)
					blk.buf_holder.reset();

				blk.write_job = nullptr;
			}
			view2.modify(piece_iter, [&blocks](cached_piece_entry& e) {
				// This is not OK. This modifies the view we're
				// iterating over
				e.flushed_cursor = compute_flushed_cursor(blocks);
			});

			TORRENT_ASSERT(m_blocks >= count);
			m_blocks -= count;
			if (piece_iter->clear_piece)
			{
				jobqueue_t aborted;
				pread_disk_job* clear_piece = nullptr;
				view2.modify(piece_iter, [&](cached_piece_entry& e) {
					clear_piece_impl(e, aborted);
					clear_piece = std::exchange(e.clear_piece, nullptr);
				});
				clear_piece_fun(std::move(aborted), clear_piece);
			}
			// if we failed to flush all blocks we wanted to, we're done
			if (count < num_blocks)
				return;
		}

		// we may still need to flush blocks at this point, even though we
		// would require read-back later to compute the piece hash
		auto& view3 = m_pieces.template get<0>();
		for (auto piece_iter = view3.begin(); piece_iter != view3.end(); ++piece_iter)
		{
#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
			INVARIANT_CHECK;
#endif

			// We avoid flushing if other threads have already initiated sufficient
			// amount of flushing
			if (m_blocks - m_flushing_blocks <= target_blocks)
				return;

			if (piece_iter->flushing)
				continue;

			TORRENT_ASSERT(piece_iter->blocks_in_piece >= 0);
			span<cached_block_entry> const blocks = piece_iter->get_blocks();
			int const num_blocks = count_jobs(blocks);
			if (num_blocks == 0) continue;

			view3.modify(piece_iter, [](cached_piece_entry& e) { TORRENT_ASSERT(!e.flushing); e.flushing = true; });

			m_flushing_blocks += num_blocks;
			int const hash_cursor = piece_iter->hasher_cursor;

			// we have to release the lock while flushing, but since we set the
			// "flushing" member to true, this piece is pinned to the cache
			l.unlock();

			int count = 0;
			{
				auto se = scope_end([&] {
					l.lock();
					view3.modify(piece_iter, [&blocks](cached_piece_entry& e) {
						e.flushing = false;
					});
					TORRENT_ASSERT(m_flushing_blocks >= num_blocks);
					m_flushing_blocks -= num_blocks;
				});
				flushed_blocks.resize(blocks.size());
				flushed_blocks.clear_all();
				count = f(flushed_blocks, blocks, hash_cursor);
			}

			// now that we hold the mutex again, we can update the entries for
			// all the blocks that were flushed
			for (int i = 0; i < blocks.size(); ++i)
			{
				if (!flushed_blocks.get_bit(i)) continue;
				cached_block_entry& blk = blocks[i];

				auto* j = blk.write_job;
				TORRENT_ASSERT(j);
				TORRENT_ASSERT(j->get_type() == aux::job_action_t::write);
				blk.buf_holder = std::move(std::get<aux::job::write>(j->action).buf);
				blk.flushed_to_disk = true;
				TORRENT_ASSERT(blk.buf_holder);
				// TODO: free these in bulk at the end, or something
				if (i < hash_cursor)
					blk.buf_holder.reset();

				blk.write_job = nullptr;
			}
			view3.modify(piece_iter, [&blocks](cached_piece_entry& e) {
				e.flushed_cursor = compute_flushed_cursor(blocks);
			});
			TORRENT_ASSERT(count <= blocks.size());
			TORRENT_ASSERT(m_blocks >= count);
			m_blocks -= count;
			if (piece_iter->clear_piece)
			{
				jobqueue_t aborted;
				pread_disk_job* clear_piece = nullptr;
				view3.modify(piece_iter, [&](cached_piece_entry& e) {
					clear_piece_impl(e, aborted);
					clear_piece = std::exchange(e.clear_piece, nullptr);
				});
				clear_piece_fun(std::move(aborted), clear_piece);
			}
			if (count < num_blocks)
				return;
		}
	}

	void flush_storage(std::function<int(bitfield&, span<cached_block_entry>, int)> f
		, storage_index_t const storage
		, std::function<void(jobqueue_t, pread_disk_job*)> clear_piece_fun)
	{
		std::unique_lock<std::mutex> l(m_mutex);

		INVARIANT_CHECK;

		auto& range_view = m_pieces.template get<0>();
		auto& piece_view = m_pieces.template get<3>();
		auto const [begin, end] = range_view.equal_range(storage, compare_storage());

		std::vector<piece_index_t> pieces;
		for (auto i = begin; i != end; ++i)
			pieces.push_back(i->piece.piece);

		bitfield flushed_blocks;

		for (auto piece : pieces)
		{
#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
			INVARIANT_CHECK;
#endif
			auto piece_iter = piece_view.find(piece_location{storage, piece});
			if (piece_iter == piece_view.end())
				continue;

			// There's a risk that some other thread is flushing this piece, but
			// won't force-flush it completely. In that case parts of the piece
			// may not be flushed
			// TODO: maybe we should track these pieces and synchronize with
			// them later. maybe wait for them to be flushed or hang our job on
			// them, but that would really only work if there's only one piece
			// left
			if (piece_iter->flushing)
				continue;

			span<cached_block_entry> const blocks = piece_iter->get_blocks();
			int const num_blocks = count_jobs(blocks);
			if (num_blocks == 0) continue;

			piece_view.modify(piece_iter, [](cached_piece_entry& e) { TORRENT_ASSERT(!e.flushing); e.flushing = true; });

			m_flushing_blocks += num_blocks;

			int const hash_cursor = piece_iter->hasher_cursor;

			// we have to release the lock while flushing, but since we set the
			// "flushing" member to true, this piece is pinned to the cache
			l.unlock();

			int count = 0;
			{
				auto se = scope_end([&] {
					l.lock();
					piece_view.modify(piece_iter, [&blocks](cached_piece_entry& e) {
						e.flushing = false;
					});
					TORRENT_ASSERT(m_flushing_blocks >= num_blocks);
					m_flushing_blocks -= num_blocks;
				});
				flushed_blocks.resize(blocks.size());
				flushed_blocks.clear_all();
				count = f(flushed_blocks, blocks, hash_cursor);
			}

			// TODO: maybe this should be done in the scope-end function
			// now that we hold the mutex again, we can update the entries for
			// all the blocks that were flushed
			for (int i = 0; i < blocks.size(); ++i)
			{
				if (!flushed_blocks.get_bit(i)) continue;
				cached_block_entry& blk = blocks[i];

				auto* j = blk.write_job;
				TORRENT_ASSERT(j);
				TORRENT_ASSERT(j->get_type() == aux::job_action_t::write);
				blk.buf_holder = std::move(std::get<aux::job::write>(j->action).buf);
				blk.flushed_to_disk = true;
				TORRENT_ASSERT(blk.buf_holder);
				// TODO: free these in bulk at the end, or something
				if (i < hash_cursor)
					blk.buf_holder.reset();

				blk.write_job = nullptr;
			}
			piece_view.modify(piece_iter, [&blocks](cached_piece_entry& e) {
				e.flushed_cursor = compute_flushed_cursor(blocks);
			});

			TORRENT_ASSERT(m_blocks >= count);
			m_blocks -= count;
			if (piece_iter->clear_piece)
			{
				jobqueue_t aborted;
				pread_disk_job* clear_piece = nullptr;
				piece_view.modify(piece_iter, [&](cached_piece_entry& e) {
					clear_piece_impl(e, aborted);
					clear_piece = std::exchange(e.clear_piece, nullptr);
				});
				clear_piece_fun(std::move(aborted), clear_piece);
			}

//			if (piece_iter->piece_hash_returned)
//			{
				TORRENT_ASSERT(!piece_iter->flushing);
				TORRENT_ASSERT(!piece_iter->hashing);
				piece_iter = piece_view.erase(piece_iter);
//			}
		}
	}

	std::size_t size() const
	{
		std::unique_lock<std::mutex> l(m_mutex);
		INVARIANT_CHECK;
		return m_blocks;
	}

	std::size_t num_flushing() const
	{
		std::unique_lock<std::mutex> l(m_mutex);
		INVARIANT_CHECK;
		return m_flushing_blocks;
	}

#if TORRENT_USE_INVARIANT_CHECKS
	void check_invariant() const
	{
		// mutex must be held by caller
		int dirty_blocks = 0;
		int clean_blocks = 0;
		int flushing_blocks = 0;

		auto& view = m_pieces.template get<2>();
		for (auto const& piece_entry : view)
		{
			int const num_blocks = piece_entry.blocks_in_piece;

			if (piece_entry.flushing)
				flushing_blocks += num_blocks;

			span<cached_block_entry> const blocks = piece_entry.get_blocks();

			TORRENT_ASSERT(piece_entry.flushed_cursor <= num_blocks);
			TORRENT_ASSERT(piece_entry.hasher_cursor <= num_blocks);

			int idx = 0;
			for (auto& be : blocks)
			{
				if (be.write_job) ++dirty_blocks;
				if (be.buf_holder) ++clean_blocks;
				// a block holds either a write job or buffer, never both
				TORRENT_ASSERT(!(bool(be.write_job) && bool(be.buf_holder)));
				if (be.write_job)
					TORRENT_ASSERT(be.write_job->get_type() == aux::job_action_t::write);

				if (idx < piece_entry.flushed_cursor)
					TORRENT_ASSERT(be.write_job == nullptr);
				else if (idx == piece_entry.flushed_cursor)
					TORRENT_ASSERT(!be.buf_holder);

//				if (idx < piece_entry.hasher_cursor)
//					TORRENT_ASSERT(!be.buf_holder);

				if (piece_entry.ready_to_flush)
					TORRENT_ASSERT(be.write_job != nullptr || be.flushed_to_disk);
				++idx;
			}
		}
		// if one or more blocks are being flushed, we cannot know how many blocks
		// are in flight. We just know the limit
		TORRENT_ASSERT(dirty_blocks == m_blocks);
		TORRENT_ASSERT(m_flushing_blocks <= flushing_blocks);
	}
#endif

private:

	// this requires the mutex to be locked
	void clear_piece_impl(cached_piece_entry& cpe, jobqueue_t& aborted)
	{
		TORRENT_ASSERT(!cpe.flushing);
		TORRENT_ASSERT(!cpe.hashing);
		for (int idx = 0; idx < cpe.blocks_in_piece; ++idx)
		{
			auto& cbe = cpe.blocks[idx];
			if (cbe.write_job)
			{
				aborted.push_back(cbe.write_job);
				cbe.write_job = nullptr;
				cbe.flushed_to_disk = false;
				--m_blocks;
			}
			cbe.buf_holder.reset();
		}
		cpe.ready_to_flush = false;
		cpe.piece_hash_returned = false;
		cpe.hasher_cursor = 0;
		cpe.flushed_cursor = 0;
		cpe.ph = hasher{};
	}

	mutable std::mutex m_mutex;
	piece_container m_pieces;

	// the number of *dirty* blocks in the cache. i.e. blocks that need to be
	// flushed to disk. The cache may (briefly) hold more buffers than this
	// while finishing hashing blocks.
	int m_blocks = 0;

	// the number of blocks currently being flushed by a disk thread
	// we use this to avoid over-shooting flushing blocks
	int m_flushing_blocks = 0;
};

}
}

#endif

