/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "proc_stats.h"
#include "process_tree.h"
#include "scheduler.h"
#include "str.h"
#include "zsim.h"

class ProcStats::ProcessCounter : public Counter {
    private:
        ProcStats* ps;

    public:
        ProcessCounter(ProcStats* _ps) : Counter(), ps(_ps) {}

        uint64_t get() const {
            ps->update();
            return Counter::get();
        }
};

class ProcStats::ProcessVectorCounter : public VectorCounter {
    private:
        ProcStats* ps;

    public:
        ProcessVectorCounter(ProcStats* _ps) : VectorCounter(), ps(_ps) {}

        uint64_t count(uint32_t idx) const {
            ps->update();
            return VectorCounter::count(idx);
        }
};

static uint64_t StatSize(Stat* s) {
    uint64_t sz = 0;
    if (s->isAggregate()) {
        AggregateStat* as = static_cast<AggregateStat*>(s);
        for (uint32_t i = 0; i < as->size(); i++) sz += StatSize(as->get(i));
    } else if (s->isScalar()) {
        sz = 1;
    } else if (s->isVector()) {
        VectorStat* vs = static_cast<VectorStat*>(s);
        sz = vs->size();
    } else {
        panic("Unrecognized stat type");
    }
    return sz;
}

static uint64_t* DumpWalk(Stat* s, uint64_t* curPtr) {
    if (s->isAggregate()) {
        AggregateStat* as = static_cast<AggregateStat*>(s);
        for (uint32_t i = 0; i < as->size(); i++) {
            curPtr = DumpWalk(as->get(i), curPtr);
        }
    } else if (s->isScalar()) {
        ScalarStat* ss = static_cast<ScalarStat*>(s);
        *(curPtr++) = ss->get();
    } else if (s->isVector()) {
        VectorStat* vs = static_cast<VectorStat*>(s);
        for (uint32_t i = 0; i < vs->size(); i++) {
            *(curPtr++) = vs->count(i);
        }
    } else {
        panic("Unrecognized stat type");
    }
    return curPtr;
}

static uint64_t* IncWalk(Stat* s, uint64_t* curPtr) {
    if (s->isAggregate()) {
        AggregateStat* as = static_cast<AggregateStat*>(s);
        for (uint32_t i = 0; i < as->size(); i++) {
            curPtr = IncWalk(as->get(i), curPtr);
        }
    } else if (s->isScalar()) {
        Counter* cs = static_cast<Counter*>(s);
        cs->inc(*(curPtr++));
    } else if (s->isVector()) {
        VectorCounter* vs = static_cast<VectorCounter*>(s);
        for (uint32_t i = 0; i < vs->size(); i++) {
            vs->inc(i, *(curPtr++));
        }
    } else {
        panic("Unrecognized stat type (this should be called on our own tree...)");
    }
    return curPtr;
}

Stat* ProcStats::replStat(Stat* s, const char* name, const char* desc) {
    if (!name) name = s->name();
    if (!desc) desc = s->desc();
    if (s->isAggregate()) {
        AggregateStat* as = static_cast<AggregateStat*>(s);
        AggregateStat* res = new AggregateStat(as->isRegular());
        res->init(name, desc);
        for (uint32_t i = 0; i < as->size(); i++) {
            res->append(replStat(as->get(i)));
        }
        return res;
    } else if (s->isScalar()) {
        Counter* res = new ProcessCounter(this);
        res->init(name, desc);
        return res;
    } else if (s->isVector()) {
        VectorStat* vs = static_cast<VectorStat*>(s);
        VectorCounter* res = new ProcessVectorCounter(this);
        assert(!vs->hasCounterNames());  // FIXME: Implement counter name copying
        res->init(name, desc, vs->size());
        return res;
    } else {
        panic("Unrecognized stat type");
        return NULL;
    }
}


ProcStats::ProcStats(AggregateStat* parentStat, AggregateStat* _coreStats) : coreStats(_coreStats) {
    uint32_t maxProcs = zinfo->lineSize;
    lastUpdatePhase = 0;

    // Check that coreStats are appropriate
    assert(coreStats);
    for (uint32_t i = 0; i < coreStats->size(); i++) {
        Stat* s = coreStats->get(i);
        if (!s->isAggregate())
            panic("Stat %s is not per-core (%s)", s->name(), "not aggregate stat");

        AggregateStat* as = static_cast<AggregateStat*>(s);
        // auto err = [s](const char* reason) {
        //     panic("Stat %s is not per-core (%s)", s->name(), reason);
        // };
        if (!as->isRegular())
            panic("Stat %s is not per-core (%s)", s->name(), "irregular aggregate");

        if (as->size() != zinfo->numCores)
            panic("Stat %s is not per-core (%s)", s->name(), "elems != cores");
    }

    // Initialize all the buffers
    bufSize = StatSize(coreStats);
    buf = gm_calloc<uint64_t>(bufSize);
    lastBuf = gm_calloc<uint64_t>(bufSize);

    // Create the procStats
    procStats = new AggregateStat(true);
    procStats->init("procStats", "Per-process stats");
    for (uint32_t p = 0; p < maxProcs; p++) {
        AggregateStat* ps = new AggregateStat(false);
        const char* name = gm_strdup(("procStats-" + Str(p)).c_str());
        ps->init(name, "Per-process stats");
        for (uint32_t i = 0; i < coreStats->size(); i++) {
            assert(coreStats->get(i)->isAggregate());
            AggregateStat* as = static_cast<AggregateStat*>(coreStats->get(i));
            assert(as->isRegular());
            ps->append(replStat(as->get(0), as->name(), as->desc()));
        }
        procStats->append(ps);
    }
    parentStat->append(procStats);
}

void ProcStats::update() {
    if (likely(lastUpdatePhase == zinfo->numPhases)) return;
    assert(lastUpdatePhase < zinfo->numPhases);

    uint64_t* endBuf = DumpWalk(coreStats, buf);
    assert(buf + bufSize == endBuf);

    for (uint64_t i = 0; i < bufSize; i++) {
        lastBuf[i] = buf[i] - lastBuf[i];
    }
    std::swap(lastBuf, buf);

    // Now lastBuf has been updated and buf has the differences of all the counters
    uint64_t start = 0;
    for (uint32_t i = 0; i < coreStats->size(); i++) {
        assert(coreStats->get(i)->isAggregate());
        AggregateStat* as = static_cast<AggregateStat*>(coreStats->get(i));

        for (uint32_t c = 0; c < as->size(); c++) {
            assert(as->get(c)->isAggregate());
            AggregateStat* cs = static_cast<AggregateStat*>(as->get(c));
            uint32_t p = zinfo->sched->getScheduledPid(c);
            if (p == (uint32_t)-1) p = zinfo->lineSize - 1;  // FIXME
            else p = zinfo->procArray[p]->getGroupIdx();
            assert(procStats->get(p)->isAggregate());
            Stat* ps = static_cast<AggregateStat*>(procStats->get(p))->get(i);
            assert(StatSize(cs) == StatSize(ps));
            IncWalk(ps, buf + start);
            start += StatSize(cs);
        }
    }
    assert(start == bufSize);

    lastUpdatePhase = zinfo->numPhases;
}

void ProcStats::notifyDeschedule() {
    // FIXME: In general this may be called mid-phase... will need synchronization then;
    // For non-overcommitted systems it works though.
    update();
}
