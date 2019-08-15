#include "tlb.h"
#include "zsim.h"

TLB::TLB(uint32_t _numLines, CC* _cc, CacheArray* _array,
         ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const g_string& _name)
    : cc(_cc), array(_array), rp(_rp), numLines(_numLines), accLat(_accLat),
      invLat(_invLat), name(_name) {
        srcId = -1;
        pageWalk = false;
        pageWalkLat = 0;
        reqFlags = MemReq::PTEFETCH;
        cc->sinkTLB(false);
    }

const char* TLB::getName()
{
    return name.c_str();
}

void TLB::setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network)
{
    cc->setParents(childId, parents, network);
}

void TLB::setChildren(const g_vector<BaseCache*>& children, Network* network)
{
    cc->setChildren(children, network);
}

void TLB::initStats(AggregateStat* parentStat)
{
    AggregateStat* tlbStat = new AggregateStat();
    tlbStat->init(name.c_str(), "TLB stats");
    initTLBStats(tlbStat);
    parentStat->append(tlbStat);
}

uint64_t TLB::translate(Address vAddr, uint64_t curCycle)
{
    Address vPageNum = vAddr >> pageBits;
    Address pPageNum = procMask | vPageNum;
    // info("Translating vAddr: %lx, vPageNum: %lx, pPageNum: %lx", vAddr, vPageNum, pPageNum);
    MESIState dummyState = MESIState::I;
    MemReq req = {pPageNum, GETS, 0, &dummyState, curCycle, nullptr, dummyState, srcId, reqFlags};
    uint64_t respCycle = access(req);
    return respCycle;
}

uint64_t TLB::access(MemReq& req)
{
    uint64_t respCycle = req.cycle;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
    }

    int32_t lineId = array->lookup(req.pageNum, nullptr, false);

    // info("[%s] Translating page 0x%lx", name.c_str(), req.pageNum);
    respCycle += accLat;
    bool accessed = false;
    if (lineId == -1) {
        /* Find the line to insert. There is no need to write back. */
        Address WbAddr;
        lineId = array->preinsert(req.pageNum, nullptr, &WbAddr);
        // info("[%s] Evict TLB entry 0x%lx for 0x%lx", name.c_str(), WbAddr, req.pageNum);
        respCycle = cc->processEviction(req, WbAddr, lineId, respCycle);

        if (pageWalk) {
            /* Generate a MemReq to get PTE. */
            Address pLineAddr = (req.pageNum / pteSize) >> lineBits;
            // info("pLineAddr: %lx", pLineAddr);
            respCycle += pageWalkLat;
            MemReq mem_req = {pLineAddr, GETS, 0, req.state, respCycle, nullptr, *req.state, req.srcId, req.flags};
            respCycle = cc->processAccess(mem_req, lineId, respCycle);
            accessed = true;
        }

        array->postinsert(req.pageNum, &req, lineId);
    }

    if (!accessed)
        respCycle = cc->processAccess(req, lineId, respCycle);

    cc->endAccess(req);

    return respCycle;
}

uint64_t TLB::invalidate(const InvReq& req)
{
    panic("TLB entry should not be invalidated by lower level caches.");
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false);
    assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    uint64_t respCycle = req.cycle + invLat;
    trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    respCycle = cc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
    trace(Cache, "[%s] Invalidate end 0x%lx type %s lineId %d, reqWriteback %d, latency %ld", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback, respCycle - req.cycle);

    return respCycle;
}

void TLB::initTLBStats(AggregateStat* tlbStat)
{
    cc->initStats(tlbStat);
    array->initStats(tlbStat);
    rp->initStats(tlbStat);
}


