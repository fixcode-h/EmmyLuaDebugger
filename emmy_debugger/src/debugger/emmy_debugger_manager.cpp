#include "emmy_debugger/debugger/emmy_debugger_manager.h"
#include "emmy_debugger/api/lua_version.h"
#include "emmy_debugger/util.h"

EmmyDebuggerManager::EmmyDebuggerManager()
	: isRunning(false)
{
}

EmmyDebuggerManager::~EmmyDebuggerManager()
{
}

std::shared_ptr<Debugger> EmmyDebuggerManager::GetDebugger(lua_State* L)
{
	EMMY_LOCK_GUARD(debuggerMtx);
	auto identify = GetUniqueIdentify(L);
	auto it = debuggers.find(identify);
	if (it != debuggers.end())
	{
		return it->second;
	}
	else
	{
		return nullptr;
	}
}

std::shared_ptr<Debugger> EmmyDebuggerManager::GetDebuggerByVmId(uint64_t vmId)
{
	if (vmId == 0) return nullptr;
	EMMY_LOCK_GUARD(debuggerMtx);
	for (auto &entry : debuggers)
	{
		if (entry.second && entry.second->GetVmId() == vmId)
		{
			return entry.second;
		}
	}
	return nullptr;
}

bool EmmyDebuggerManager::BindVmId(lua_State* L, uint64_t vmId)
{
	if (vmId == 0) return false;
	EMMY_LOCK_GUARD(debuggerMtx);
	const auto identify = GetUniqueIdentify(L);
	auto it = debuggers.find(identify);
	if (it == debuggers.end() || !it->second) return false;
	for (auto &entry : debuggers)
	{
		if (entry.first != identify && entry.second && entry.second->GetVmId() == vmId)
		{
			return false;
		}
	}
	it->second->SetVmId(vmId);
	return true;
}

std::shared_ptr<Debugger> EmmyDebuggerManager::AddDebugger(lua_State* L)
{
	EMMY_LOCK_GUARD(debuggerMtx);

	auto identify = GetUniqueIdentify(L);

	std::shared_ptr<Debugger> debugger = nullptr;

	auto it = debuggers.find(identify);

	if (it == debuggers.end())
	{
		if (luaVersion != LuaVersion::LUA_JIT)
		{
			debugger = std::make_shared<Debugger>(reinterpret_cast<lua_State*>(identify), this);
		}
		else
		{
			// 如果首次add 的state不是main state，则main state视为空指针
			// 但不影响luajit附加调试和远程调试
			lua_State* mainState = nullptr;

			int ret = lua_pushthread(L);
			lua_pop(L, 1);
			if (ret == 1)
			{
				mainState = L;
			}

			debugger = std::make_shared<Debugger>(mainState, this);
		}
		debuggers.insert({identify, debugger});
	}
	else
	{
		debugger = it->second;
	}

	debugger->SetCurrentState(L);
	return debugger;
}


std::shared_ptr<Debugger> EmmyDebuggerManager::RemoveDebugger(lua_State* L)
{
	EMMY_LOCK_GUARD(debuggerMtx);
	auto identify = GetUniqueIdentify(L);
	auto it = debuggers.find(identify);
	if (it != debuggers.end())
	{
		auto debugger = it->second;
		debuggers.erase(it);
		return debugger;
	}
	return nullptr;
}

std::vector<std::shared_ptr<Debugger>> EmmyDebuggerManager::GetDebuggers()
{
	EMMY_LOCK_GUARD(debuggerMtx);
	std::vector<std::shared_ptr<Debugger>> debuggerVector;
	for (auto it : debuggers)
	{
		debuggerVector.push_back(it.second);
	}
	return debuggerVector;
}

void EmmyDebuggerManager::RemoveAllDebugger()
{
	EMMY_LOCK_GUARD(debuggerMtx);
	debuggers.clear();
	{
		EMMY_LOCK_GUARD(breakDebuggerMtx);
		hitDebugger.reset();
	}
}

std::shared_ptr<Debugger> EmmyDebuggerManager::GetHitBreakpoint()
{
	EMMY_LOCK_GUARD(breakDebuggerMtx);
	return hitDebugger;
}

std::shared_ptr<Debugger> EmmyDebuggerManager::RemoveDebuggerByVmId(uint64_t vmId) {
	EMMY_LOCK_GUARD(debuggerMtx);
	for (auto it = debuggers.begin(); it != debuggers.end(); ++it) {
		if (it->second && it->second->GetVmId() == vmId) {
			auto result = it->second;
			debuggers.erase(it);
			return result;
		}
	}
	return nullptr;
}

void EmmyDebuggerManager::ClearHitDebugger(uint64_t vmId)
{
	EMMY_LOCK_GUARD(breakDebuggerMtx);
	if (!hitDebugger || vmId == 0 || hitDebugger->GetVmId() == vmId) {
		hitDebugger.reset();
	}
}

void EmmyDebuggerManager::SetHitDebugger(std::shared_ptr<Debugger> debugger)
{
	EMMY_LOCK_GUARD(breakDebuggerMtx);
	hitDebugger = debugger;
}

bool EmmyDebuggerManager::IsDebuggerEmpty()
{
	EMMY_LOCK_GUARD(debuggerMtx);
	return debuggers.empty();
}

namespace {

std::string BreakpointLocation(const BreakPoint& breakpoint) {
	return breakpoint.sourceCanonicalPath.empty()
		? breakpoint.file : breakpoint.sourceCanonicalPath;
}

bool SameBreakpointLocation(const BreakPoint& left, const BreakPoint& right) {
	return left.line == right.line && left.vmId == right.vmId &&
		CompareIgnoreCase(BreakpointLocation(left), BreakpointLocation(right)) == 0;
}

BreakPointContribution ContributionFromBreakpoint(const BreakPoint& breakpoint) {
	BreakPointContribution contribution;
	contribution.owner = breakpoint.owner.empty() ? "LEGACY" : breakpoint.owner;
	contribution.breakpointId = breakpoint.breakpointId;
	contribution.condition = breakpoint.condition;
	contribution.logMessage = breakpoint.logMessage;
	contribution.hitCondition = breakpoint.hitCondition;
	contribution.hitCount = breakpoint.hitCount;
	contribution.runToHere = breakpoint.runToHere;
	return contribution;
}

void RefreshCompositeFields(BreakPoint& breakpoint) {
	// The legacy evaluator consumes the top-level fields. Keep them as a
	// compatibility projection of the first contribution; the full list is
	// retained for reason calculation and per-owner hit counters.
	breakpoint.condition.clear();
	breakpoint.logMessage.clear();
	breakpoint.hitCondition.clear();
	breakpoint.hitCount = 0;
	breakpoint.runToHere = false;
	if (!breakpoint.contributions.empty()) {
		const BreakPointContribution& first = breakpoint.contributions.front();
		breakpoint.condition = first.condition;
		breakpoint.logMessage = first.logMessage;
		breakpoint.hitCondition = first.hitCondition;
		breakpoint.hitCount = first.hitCount;
		breakpoint.runToHere = first.runToHere;
	}
}

void DeduplicateContributions(BreakPoint& breakpoint) {
	std::vector<BreakPointContribution> unique;
	std::map<std::string, std::size_t> indexes;
	for (std::vector<BreakPointContribution>::const_iterator it = breakpoint.contributions.begin();
			it != breakpoint.contributions.end(); ++it) {
		const std::string key = it->owner + std::string(1, '\x1f') + it->breakpointId;
		std::map<std::string, std::size_t>::iterator existing = indexes.find(key);
		if (existing == indexes.end()) {
			indexes[key] = unique.size();
			unique.push_back(*it);
		} else {
			// Last writer wins while preserving the first-seen ordering used by
			// the legacy top-level projection and pause reason list.
			unique[existing->second] = *it;
		}
	}
	breakpoint.contributions.swap(unique);
}

void NormalizeBreakpoint(BreakPoint& breakpoint) {
	if (breakpoint.sourceCanonicalPath.empty()) breakpoint.sourceCanonicalPath = breakpoint.file;
	if (breakpoint.file.empty()) breakpoint.file = breakpoint.sourceCanonicalPath;
	if (breakpoint.contributions.empty()) {
		breakpoint.contributions.push_back(ContributionFromBreakpoint(breakpoint));
	}
	DeduplicateContributions(breakpoint);
	RefreshCompositeFields(breakpoint);
}

} // namespace

void EmmyDebuggerManager::AddBreakpoint(std::shared_ptr<BreakPoint> breakpoint)
{
	if (!breakpoint) return;
	EMMY_LOCK_GUARD(breakpointsMtx);
	NormalizeBreakpoint(*breakpoint);
	for (std::shared_ptr<BreakPoint>& existing : breakpoints) {
		if (!existing || !SameBreakpointLocation(*existing, *breakpoint)) continue;
		if (breakpoint->composite) {
			*existing = *breakpoint;
			RefreshCompositeFields(*existing);
			RefreshLineSet();
			return;
		}
		for (std::vector<BreakPointContribution>::const_iterator incoming =
				breakpoint->contributions.begin(); incoming != breakpoint->contributions.end(); ++incoming) {
			bool replaced = false;
			for (std::vector<BreakPointContribution>::iterator current = existing->contributions.begin();
					current != existing->contributions.end(); ++current) {
				const bool sameOwner = current->owner == incoming->owner;
				const bool sameId = incoming->breakpointId.empty() ||
					current->breakpointId == incoming->breakpointId;
				if (sameOwner && sameId) {
					BreakPointContribution replacement = *incoming;
					// An update that omits hitCount must not reset accumulated hits.
					if (replacement.hitCount == 0) replacement.hitCount = current->hitCount;
					*current = replacement;
					replaced = true;
					break;
				}
			}
			if (!replaced) existing->contributions.push_back(*incoming);
		}
		RefreshCompositeFields(*existing);
		RefreshLineSet();
		return;
	}
	breakpoints.push_back(breakpoint);
	RefreshLineSet();
}

void EmmyDebuggerManager::ReplaceBreakpoints(
	const std::vector<std::shared_ptr<BreakPoint>>& newBreakpoints)
{
	EMMY_LOCK_GUARD(breakpointsMtx);
	std::vector<std::shared_ptr<BreakPoint>> normalized;
	for (std::vector<std::shared_ptr<BreakPoint>>::const_iterator it = newBreakpoints.begin();
		 it != newBreakpoints.end(); ++it) {
		if (!*it) continue;
		std::shared_ptr<BreakPoint> copy(new BreakPoint(**it));
		copy->composite = true;
		NormalizeBreakpoint(*copy);
		bool merged = false;
		for (std::vector<std::shared_ptr<BreakPoint>>::iterator current = normalized.begin();
			 current != normalized.end(); ++current) {
			if (SameBreakpointLocation(**current, *copy)) {
				for (std::vector<BreakPointContribution>::const_iterator contribution =
						copy->contributions.begin(); contribution != copy->contributions.end(); ++contribution) {
					(*current)->contributions.push_back(*contribution);
				}
				DeduplicateContributions(**current);
				RefreshCompositeFields(**current);
				merged = true;
				break;
			}
		}
		if (!merged) normalized.push_back(copy);
	}
	breakpoints.swap(normalized);
	RefreshLineSet();
}

std::vector<std::shared_ptr<BreakPoint>> EmmyDebuggerManager::GetBreakpoints()
{
	EMMY_LOCK_GUARD(breakpointsMtx);
	std::vector<std::shared_ptr<BreakPoint>> result;
	result.reserve(breakpoints.size());
	for (std::vector<std::shared_ptr<BreakPoint>>::const_iterator it = breakpoints.begin();
			it != breakpoints.end(); ++it) {
		if (*it) result.push_back(std::shared_ptr<BreakPoint>(new BreakPoint(**it)));
	}
	return result;
}

void EmmyDebuggerManager::RemoveBreakpoint(const std::string& file, int line,
	const std::string& owner, const std::string& breakpointId, uint64_t vmId)
{
	EMMY_LOCK_GUARD(breakpointsMtx);
	for (std::vector<std::shared_ptr<BreakPoint>>::iterator it = breakpoints.begin();
		 it != breakpoints.end();) {
		const std::shared_ptr<BreakPoint>& bp = *it;
		if (!bp || bp->line != line || (vmId != 0 && bp->vmId != vmId) ||
			CompareIgnoreCase(BreakpointLocation(*bp), file) != 0) {
			++it;
			continue;
		}
		if (owner.empty() && breakpointId.empty()) {
			it = breakpoints.erase(it);
			continue;
		}
		for (std::vector<BreakPointContribution>::iterator contribution = bp->contributions.begin();
			 contribution != bp->contributions.end();) {
			const bool ownerMatches = owner.empty() || contribution->owner == owner;
			const bool idMatches = breakpointId.empty() || contribution->breakpointId == breakpointId;
			if (ownerMatches && idMatches) contribution = bp->contributions.erase(contribution);
			else ++contribution;
		}
		if (bp->contributions.empty()) it = breakpoints.erase(it);
		else { RefreshCompositeFields(*bp); ++it; }
	}
	RefreshLineSet();
}

void EmmyDebuggerManager::RemoveAllBreakpoints()
{
	EMMY_LOCK_GUARD(breakpointsMtx);
	breakpoints.clear();
	lineSet.clear();
}

void EmmyDebuggerManager::RefreshLineSet()
{
	lineSet.clear();
	for (auto bp : breakpoints)
	{
		lineSet.insert(bp->line);
	}
}

std::set<int> EmmyDebuggerManager::GetLineSet()
{
	EMMY_LOCK_GUARD(breakpointsMtx);
	return lineSet;
}

void EmmyDebuggerManager::HandleBreak(lua_State* L)
{
	auto debugger = GetDebugger(L);

	if (debugger)
	{
		debugger->SetCurrentState(L);
	}
	else
	{
		debugger = AddDebugger(L);
	}

	SetHitDebugger(debugger);

	debugger->HandleBreak(L);
}

void EmmyDebuggerManager::DoAction(DebugAction action)
{
	std::shared_ptr<Debugger> debugger;
	{
		EMMY_LOCK_GUARD(debuggerMtx);
		if (debuggers.size() == 1) debugger = debuggers.begin()->second;
	}
	if (debugger)
	{
		debugger->RequestAction(action, debugger->GetPauseId());
	}
}

void EmmyDebuggerManager::Eval(std::shared_ptr<EvalContext> ctx)
{
	std::shared_ptr<Debugger> debugger;
	{
		EMMY_LOCK_GUARD(debuggerMtx);
		if (debuggers.size() == 1) debugger = debuggers.begin()->second;
	}
	if (debugger)
	{
		debugger->Eval(ctx, false);
	}
}

bool EmmyDebuggerManager::DoActionForVm(uint64_t vmId, DebugAction action, uint64_t pauseId)
{
	auto debugger = GetDebuggerByVmId(vmId);
	if (!debugger || (pauseId != 0 && !debugger->IsPauseActive(pauseId))) return false;
	if (action != DebugAction::Break && pauseId == 0) return false;
	return debugger->RequestAction(action, pauseId);
}

bool EmmyDebuggerManager::EvalForVm(uint64_t vmId, std::shared_ptr<EvalContext> ctx)
{
	auto debugger = GetDebuggerByVmId(vmId);
	if (!debugger) return false;
	if (!ctx || ctx->pauseId == 0 || !debugger->IsPauseActive(ctx->pauseId)) return false;
	return debugger->Eval(ctx, false);
}

EmmyDebuggerManager::RouteResult EmmyDebuggerManager::RouteAction(
	uint64_t vmId, DebugAction action, uint64_t pauseId, const std::string& threadId,
	uint64_t contextGeneration, uint64_t sourceEpoch) {
	if (vmId == 0) {
		EMMY_LOCK_GUARD(debuggerMtx);
		return debuggers.size() == 1
			? RouteResult{false, "VM_ID_REQUIRED"}
			: RouteResult{false, debuggers.empty() ? "VM_NOT_FOUND" : "AMBIGUOUS_VM"};
	}
	if (!GetDebuggerByVmId(vmId)) return RouteResult{false, "VM_NOT_FOUND"};
	if (action != DebugAction::Break && pauseId == 0) return RouteResult{false, "PAUSE_ID_REQUIRED"};
	auto debugger = GetDebuggerByVmId(vmId);
	if (debugger && contextGeneration != 0 &&
		contextGeneration != debugger->GetContextGeneration()) {
		return RouteResult{false, "STALE_CONTEXT"};
	}
	if (debugger && sourceEpoch != 0 && sourceEpoch != debugger->GetSourceEpoch()) {
		return RouteResult{false, "STALE_SOURCE_EPOCH"};
	}
	if (debugger && !threadId.empty() && !debugger->IsPauseThread(threadId)) {
		return RouteResult{false, "STALE_PAUSE_REFERENCE"};
	}
	if (!DoActionForVm(vmId, action, pauseId)) return RouteResult{false, "STALE_PAUSE_REFERENCE"};
	return RouteResult{true, nullptr};
}

EmmyDebuggerManager::RouteResult EmmyDebuggerManager::RouteEval(
	uint64_t vmId, std::shared_ptr<EvalContext> ctx) {
	if (vmId == 0) return RouteResult{false, "VM_ID_REQUIRED"};
	if (!GetDebuggerByVmId(vmId)) return RouteResult{false, "VM_NOT_FOUND"};
	if (!ctx || ctx->pauseId == 0) return RouteResult{false, "PAUSE_ID_REQUIRED"};
	auto debugger = GetDebuggerByVmId(vmId);
	if (ctx->stackLevel < 0 || ctx->frameId.empty() ||
		ctx->frameId != debugger->GetPauseFrameId(ctx->stackLevel)) {
		return RouteResult{false, "STALE_PAUSE_REFERENCE"};
	}
	if (debugger && ((!ctx->threadId.empty() && !debugger->IsPauseThread(ctx->threadId)) ||
		!debugger->IsFrameActive(ctx->frameId) ||
		(ctx->contextGeneration != 0 &&
			ctx->contextGeneration != debugger->GetContextGeneration()) ||
		(ctx->sourceEpoch != 0 && ctx->sourceEpoch != debugger->GetSourceEpoch()))) {
		if (ctx->contextGeneration != 0 &&
			ctx->contextGeneration != debugger->GetContextGeneration()) {
			return RouteResult{false, "STALE_CONTEXT"};
		}
		if (ctx->sourceEpoch != 0 && ctx->sourceEpoch != debugger->GetSourceEpoch()) {
			return RouteResult{false, "STALE_SOURCE_EPOCH"};
		}
		return RouteResult{false, "STALE_PAUSE_REFERENCE"};
	}
	if (!EvalForVm(vmId, ctx)) return RouteResult{false, "STALE_PAUSE_REFERENCE"};
	return RouteResult{true, nullptr};
}

void EmmyDebuggerManager::OnDisconnect()
{
	SetRunning(false);
	EMMY_LOCK_GUARD(debuggerMtx);
	for (auto it : debuggers)
	{
		// Release any pause before stopping the debugger. EnterDebugMode only
		// wakes on an action, so a dropped connection (debug window closed,
		// transport error, agent reconfigure) would otherwise leave the host's
		// Lua thread blocked inside the pause loop forever: the target stays
		// frozen with no client able to resume it.
		it.second->ClearPause();
		it.second->ExitDebugMode();
		it.second->Stop();
	}
}

void EmmyDebuggerManager::SetRunning(bool value)
{
	isRunning = value;
	if(isRunning)
	{
		for(auto debugger: GetDebuggers())
		{
			debugger->Start();
			// 重新连接时也需要执行 helperCode
			debugger->Attach();
		}
	}
}

bool EmmyDebuggerManager::IsRunning()
{
	return isRunning;
}

EmmyDebuggerManager::UniqueIdentifyType EmmyDebuggerManager::GetUniqueIdentify(lua_State* L)
{
	if (luaVersion == LuaVersion::LUA_JIT)
	{
		// 我们认为luajit只会有一个debugger
		return 0;
	}
	else
	{
		return reinterpret_cast<UniqueIdentifyType>(GetMainState(L));
	}
}
