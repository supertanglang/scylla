/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <memory>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <boost/range/adaptor/map.hpp>

#include <core/future.hh>
#include <core/sharded.hh>

#include "commitlog.hh"
#include "commitlog_replayer.hh"
#include "database.hh"
#include "sstables/sstables.hh"
#include "db/system_keyspace.hh"
#include "cql3/query_processor.hh"
#include "log.hh"
#include "converting_mutation_partition_applier.hh"
#include "schema_registry.hh"
#include "commitlog_entry.hh"

static logging::logger logger("commitlog_replayer");

class db::commitlog_replayer::impl {
    struct column_mappings {
        std::unordered_map<table_schema_version, column_mapping> map;
        future<> stop() { return make_ready_future<>(); }
    };

    // we want the processing methods to be const, since they use
    // shard-sharing of data -> read only
    // this one is special since it is thread local.
    // Should actually make sharded::local a const function (it does
    // not modify content), but...
    mutable seastar::sharded<column_mappings> _column_mappings;

    friend class db::commitlog_replayer;
public:
    impl(seastar::sharded<cql3::query_processor>& db);

    future<> init();

    struct stats {
        uint64_t invalid_mutations = 0;
        uint64_t skipped_mutations = 0;
        uint64_t applied_mutations = 0;
        uint64_t corrupt_bytes = 0;

        stats& operator+=(const stats& s) {
            invalid_mutations += s.invalid_mutations;
            skipped_mutations += s.skipped_mutations;
            applied_mutations += s.applied_mutations;
            corrupt_bytes += s.corrupt_bytes;
            return *this;
        }
        stats operator+(const stats& s) const {
            stats tmp = *this;
            tmp += s;
            return tmp;
        }
    };

    // move start/stop of the thread local bookkeep to "top level"
    // and also make sure to assert on it actually being started.
    future<> start() {
        return _column_mappings.start();
    }
    future<> stop() {
        return _column_mappings.stop();
    }

    future<> process(stats*, temporary_buffer<char> buf, replay_position rp) const;
    future<stats> recover(sstring file) const;

    typedef std::unordered_map<utils::UUID, replay_position> rp_map;
    typedef std::unordered_map<unsigned, rp_map> shard_rpm_map;
    typedef std::unordered_map<unsigned, replay_position> shard_rp_map;

    seastar::sharded<cql3::query_processor>&
        _qp;
    shard_rpm_map
        _rpm;
    shard_rp_map
        _min_pos;
};

db::commitlog_replayer::impl::impl(seastar::sharded<cql3::query_processor>& qp)
    : _qp(qp)
{}

future<> db::commitlog_replayer::impl::init() {
    return _qp.map_reduce([this](shard_rpm_map map) {
        for (auto& p1 : map) {
            for (auto& p2 : p1.second) {
                auto& pp = _rpm[p1.first][p2.first];
                pp = std::max(pp, p2.second);

                auto i = _min_pos.find(p1.first);
                if (i == _min_pos.end() || p2.second < i->second) {
                    _min_pos[p1.first] = p2.second;
                }
            }
        }
    }, [this](cql3::query_processor& qp) {
        return do_with(shard_rpm_map{}, [this, &qp](shard_rpm_map& map) {
            return parallel_for_each(qp.db().local().get_column_families(), [&map, &qp](auto& cfp) {
                auto uuid = cfp.first;
                for (auto& sst : *cfp.second->get_sstables()) {
                    try {
                        auto p = sst->get_stats_metadata().position;
                        logger.trace("sstable {} -> rp {}", sst->get_filename(), p);
                        auto& pp = map[p.shard_id()][uuid];
                        pp = std::max(pp, p);
                    } catch (...) {
                        logger.warn("Could not read sstable metadata {}", std::current_exception());
                    }
                }
                // We do this on each cpu, for each CF, which technically is a little wasteful, but the values are
                // cached, this is only startup, and it makes the code easier.
                // Get all truncation records for the CF and initialize max rps if
                // present. Cannot do this on demand, as there may be no sstables to
                // mark the CF as "needed".
                return db::system_keyspace::get_truncated_position(uuid).then([&map, &uuid](std::vector<db::replay_position> tpps) {
                    for (auto& p : tpps) {
                        logger.trace("CF {} truncated at {}", uuid, p);
                        auto& pp = map[p.shard_id()][uuid];
                        pp = std::max(pp, p);
                    }
                });
            }).then([&map] {
                return make_ready_future<shard_rpm_map>(map);
            });
        });
    }).finally([this] {
        // bugfix: the above map-reduce will not_ detect if sstables
        // are _missing_ from a CF. And because of re-sharding, we can't
        // just insert initial zeros into the maps, because we don't know
        // how many shards there we're last time.
        // However, this only affects global min pos, since
        // for each CF, the worst that happens is that we have a missing
        // entry -> empty replay_pos == min value. But calculating
        // global min pos will be off, since we will only base it on
        // existing sstables-per-shard.
        // So, go through all CF:s and check, if a shard mapping does not
        // have data for it, assume we must set global pos to zero.
        for (auto&p : _qp.local().db().local().get_column_families()) {
            for (auto&p1 : _rpm) { // for each shard
                if (!p1.second.count(p.first)) {
                    _min_pos[p1.first] = replay_position();
                }
            }
        }

        for (auto&p : _min_pos) {
            logger.debug("minimum position for shard {}: {}", p.first, p.second);
        }
        for (auto&p1 : _rpm) {
            for (auto& p2 : p1.second) {
                logger.debug("replay position for shard/uuid {}/{}: {}", p1.first, p2.first, p2.second);
            }
        }
    });
}

future<db::commitlog_replayer::impl::stats>
db::commitlog_replayer::impl::recover(sstring file) const {
    assert(_column_mappings.local_is_initialized());

    replay_position rp{commitlog::descriptor(file)};
    auto gp = _min_pos.at(rp.shard_id());

    if (rp.id < gp.id) {
        logger.debug("skipping replay of fully-flushed {}", file);
        return make_ready_future<stats>();
    }
    position_type p = 0;
    if (rp.id == gp.id) {
        p = gp.pos;
    }

    auto s = make_lw_shared<stats>();

    return db::commitlog::read_log_file(file,
            std::bind(&impl::process, this, s.get(), std::placeholders::_1,
                    std::placeholders::_2), p).then([](auto s) {
        auto f = s->done();
        return f.finally([s = std::move(s)] {});
    }).then_wrapped([s](future<> f) {
        try {
            f.get();
        } catch (commitlog::segment_data_corruption_error& e) {
            s->corrupt_bytes += e.bytes();
        } catch (...) {
            throw;
        }
        return make_ready_future<stats>(*s);
    });
}

future<> db::commitlog_replayer::impl::process(stats* s, temporary_buffer<char> buf, replay_position rp) const {
    try {

        commitlog_entry_reader cer(buf);
        auto& fm = cer.mutation();

        auto& local_cm = _column_mappings.local().map;
        auto cm_it = local_cm.find(fm.schema_version());
        if (cm_it == local_cm.end()) {
            if (!cer.get_column_mapping()) {
                throw std::runtime_error(sprint("unknown schema version {}", fm.schema_version()));
            }
            logger.debug("new schema version {} in entry {}", fm.schema_version(), rp);
            cm_it = local_cm.emplace(fm.schema_version(), *cer.get_column_mapping()).first;
        }
        const column_mapping& src_cm = cm_it->second;

        auto shard_id = rp.shard_id();
        if (rp < _min_pos.at(shard_id)) {
            logger.trace("entry {} is less than global min position. skipping", rp);
            s->skipped_mutations++;
            return make_ready_future<>();
        }

        auto uuid = fm.column_family_id();
        auto& map = _rpm.at(shard_id);
        auto i = map.find(uuid);
        if (i != map.end() && rp <= i->second) {
            logger.trace("entry {} at {} is younger than recorded replay position {}. skipping", fm.column_family_id(), rp, i->second);
            s->skipped_mutations++;
            return make_ready_future<>();
        }

        auto shard = _qp.local().db().local().shard_of(fm);
        return _qp.local().db().invoke_on(shard, [this, cer = std::move(cer), &src_cm, rp, shard, s] (database& db) -> future<> {
            auto& fm = cer.mutation();
            // TODO: might need better verification that the deserialized mutation
            // is schema compatible. My guess is that just applying the mutation
            // will not do this.
            auto& cf = db.find_column_family(fm.column_family_id());

            if (logger.is_enabled(logging::log_level::debug)) {
                logger.debug("replaying at {} v={} {}:{} at {}", fm.column_family_id(), fm.schema_version(),
                        cf.schema()->ks_name(), cf.schema()->cf_name(), rp);
            }
            // Removed forwarding "new" RP. Instead give none/empty.
            // This is what origin does, and it should be fine.
            // The end result should be that once sstables are flushed out
            // their "replay_position" attribute will be empty, which is
            // lower than anything the new session will produce.
            if (cf.schema()->version() != fm.schema_version()) {
                auto& local_cm = _column_mappings.local().map;
                auto cm_it = local_cm.find(fm.schema_version());
                if (cm_it == local_cm.end()) {
                    cm_it = local_cm.emplace(fm.schema_version(), src_cm).first;
                }
                const column_mapping& cm = cm_it->second;
                mutation m(fm.decorated_key(*cf.schema()), cf.schema());
                converting_mutation_partition_applier v(cm, *cf.schema(), m.partition());
                fm.partition().accept(cm, v);
                cf.apply(std::move(m));
            } else {
                cf.apply(fm, cf.schema());
            }
            s->applied_mutations++;
            return make_ready_future<>();
        }).handle_exception([s](auto ep) {
            s->invalid_mutations++;
            // TODO: write mutation to file like origin.
            logger.warn("error replaying: {}", ep);
        });
    } catch (no_such_column_family&) {
        // No such CF now? Origin just ignores this.
    } catch (...) {
        s->invalid_mutations++;
        // TODO: write mutation to file like origin.
        logger.warn("error replaying: {}", std::current_exception());
    }

    return make_ready_future<>();
}

db::commitlog_replayer::commitlog_replayer(seastar::sharded<cql3::query_processor>& qp)
    : _impl(std::make_unique<impl>(qp))
{}

db::commitlog_replayer::commitlog_replayer(commitlog_replayer&& r) noexcept
    : _impl(std::move(r._impl))
{}

db::commitlog_replayer::~commitlog_replayer()
{}

future<db::commitlog_replayer> db::commitlog_replayer::create_replayer(seastar::sharded<cql3::query_processor>& qp) {
    return do_with(commitlog_replayer(qp), [](auto&& rp) {
        auto f = rp._impl->init();
        return f.then([rp = std::move(rp)]() mutable {
            return make_ready_future<commitlog_replayer>(std::move(rp));
        });
    });
}

future<> db::commitlog_replayer::recover(std::vector<sstring> files) {
    typedef std::unordered_multimap<unsigned, sstring> shard_file_map;

    logger.info("Replaying {}", join(", ", files));

    // pre-compute work per shard already.
    auto map = ::make_lw_shared<shard_file_map>();
    for (auto& f : files) {
        commitlog::descriptor d(f);
        replay_position p = d;
        map->emplace(p.shard_id() % smp::count, std::move(f));
    }

    return _impl->start().then([this, map] {
        return map_reduce(smp::all_cpus(), [this, map](unsigned id) {
            return smp::submit_to(id, [this, id, map]() {
                auto total = ::make_lw_shared<impl::stats>();
                // TODO: or something. For now, we do this serialized per shard,
                // to reduce mutation congestion. We could probably (says avi)
                // do 2 segments in parallel or something, but lets use this first.
                return do_for_each(map->begin(id), map->end(id), [this, total](const std::pair<unsigned, sstring>& p) {
                    auto&f = p.second;
                    logger.debug("Replaying {}", f);
                    return _impl->recover(f).then([f, total](impl::stats stats) {
                        if (stats.corrupt_bytes != 0) {
                            logger.warn("Corrupted file: {}. {} bytes skipped.", f, stats.corrupt_bytes);
                        }
                        logger.debug("Log replay of {} complete, {} replayed mutations ({} invalid, {} skipped)"
                                        , f
                                        , stats.applied_mutations
                                        , stats.invalid_mutations
                                        , stats.skipped_mutations
                        );
                        *total += stats;
                    });
                }).then([total] {
                    return make_ready_future<impl::stats>(*total);
                });
            });
        }, impl::stats(), std::plus<impl::stats>()).then([](impl::stats totals) {
            logger.info("Log replay complete, {} replayed mutations ({} invalid, {} skipped)"
                            , totals.applied_mutations
                            , totals.invalid_mutations
                            , totals.skipped_mutations
            );
        });
    }).finally([this] {
        return _impl->stop();
    });
}

future<> db::commitlog_replayer::recover(sstring f) {
    return recover(std::vector<sstring>{ f });
}

