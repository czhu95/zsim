#include "tlb.h"
#include "zsim.h"

TLB::TLB(uint32_t _numLines, CC* _cc, CacheArray* _array,
         ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const g_string& _name)
    : cc(_cc), array(_array), rp(_rp), numLines(_numLines), accLat(_accLat),
      invLat(_invLat), name(_name) {
        srcId = -1;
        reqFlags = MemReq::PTEFETCH;
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
    TLBReq req = {pPageNum, curCycle, 0};
    uint64_t respCycle = access(req);
    return respCycle;
}

uint64_t TLB::access(TLBReq& req)
{
    uint64_t respCycle = req.cycle;
    int32_t lineId = array->lookup(req.pageNum, nullptr, false);
    respCycle += accLat;
    if (lineId == -1) {
        // info("TLB miss, pageNum: %lx, lineId: %d", req.pageNum, lineId);
        /* Find the line to insert. There is no need to write back. */
        Address WbAddr;
        lineId = array->preinsert(req.pageNum, nullptr, &WbAddr);

        /* Generate a MemReq to get PTE. */
        Address pLineAddr = (req.pageNum / pteSize) >> lineBits;
        // info("pLineAddr: %lx", pLineAddr);
        respCycle += pageWalkLat;
        MESIState dummyState = MESIState::I;
        MemReq mem_req = {pLineAddr, GETS, 0, &dummyState, respCycle, nullptr, dummyState, srcId, reqFlags};

        array->postinsert(req.pageNum, &mem_req, lineId);

        bool skipAccess = cc->startAccess(mem_req);
        assert(!skipAccess);
        respCycle = cc->processAccess(mem_req, lineId, respCycle);
        cc->endAccess(mem_req);
    }

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


