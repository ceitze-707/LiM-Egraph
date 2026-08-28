#pragma once
#include <iostream>
#include <chrono>
#include <time.h>
#include <vector>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include<string>
#include<fstream>
#include <cmath>
#include<stack>
#include <algorithm>
#include <cassert>
#include <random>
#include <stdexcept>
// [06-MOD] 不再包含 mockturtle 的全量聚合头。它会连带引入 percy/nauty
// 的遗留 C 接口，在当前 MSVC 环境与本项目的 Node 类型发生解析冲突。
// 以下仅保留 IMCCompiler 的 Verilog/XMG 优化流程实际使用的组件。
#include "mockturtle/algorithms/balancing.hpp"
#include "mockturtle/algorithms/balancing/sop_balancing.hpp"
#include "mockturtle/algorithms/circuit_validator.hpp"
#include "mockturtle/algorithms/cleanup.hpp"
#include "mockturtle/algorithms/collapse_mapped.hpp"
#include "mockturtle/algorithms/cut_rewriting.hpp"
#include "mockturtle/algorithms/functional_reduction.hpp"
#include "mockturtle/algorithms/lut_mapping.hpp"
#include "mockturtle/algorithms/node_resynthesis.hpp"
#include "mockturtle/algorithms/node_resynthesis/akers.hpp"
#include "mockturtle/algorithms/node_resynthesis/xmg_npn.hpp"
#include "mockturtle/algorithms/pattern_generation.hpp"
#include "mockturtle/algorithms/refactoring.hpp"
#include "mockturtle/algorithms/simulation.hpp"
#include "mockturtle/algorithms/xmg_algebraic_rewriting.hpp"
#include "mockturtle/algorithms/xmg_resub.hpp"
#include "mockturtle/io/verilog_reader.hpp"
#include "mockturtle/networks/klut.hpp"
#include "mockturtle/networks/xmg.hpp"
#include "mockturtle/views/depth_view.hpp"
#include "mockturtle/views/fanout_view.hpp"
#include "mockturtle/views/mapping_view.hpp"
#include "lorina\aiger.hpp"

// [06-MOD] percy/nauty 旧依赖已在全局命名空间中占用 Node。用预处理器把
// baseline 内部的 Node 类型统一改名为 ImcNode，避免触碰大量调度逻辑。
// 该宏仅在本项目源文件包含 NetList.h 前生效；第三方头已全部包含完毕。
#undef Node
#define Node ImcNode

// [06-MOD] 原 baseline 写死 C 盘安装路径；06 使用本机实际依赖位置。
#include "D:\\gurobi1001\\win64\\include\\gurobi_c++.h"
#include "D:\\DESKBOOK\\0809\\z3-5.0.0-x64-win\\z3-5.0.0-x64-win\\include\\z3++.h"
#pragma comment(lib, "D:\\DESKBOOK\\0809\\z3-5.0.0-x64-win\\z3-5.0.0-x64-win\\bin\\libz3.lib")
#pragma comment(lib, "gurobi100.lib")
#pragma comment(lib, "gurobi_c++md2017.lib")

using namespace mockturtle;
using namespace std;

inline xmg_network Aig2Xmg(string strAigName)
{
	// [06-MOD] 原映射代码依赖已移除的 mockturtle exact_library API。
	// 本阶段只验证 Verilog 子网；若误用 AIG，明确失败而非静默产生空网络。
	throw runtime_error("AIG input is not supported by the 06 IMC-egraph prototype: " + strAigName);
}

inline xmg_network Bliff2Xmg(string strBliffName)
{
	// [06-MOD] 同上；保留函数签名，避免影响 Verilog 路径的调用点。
	throw runtime_error("BLIF input is not supported by the 06 IMC-egraph prototype: " + strBliffName);
}

inline void Trim(string& s)
{//remove ' ' and '\\'
	s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
	s.erase(std::remove(s.begin(), s.end(), '\\'), s.end());
}

inline string ExtractStr(string& s, string strLeft, string strRight)
{//extract substring from strLeft to strRight in s
	size_t left = s.find(strLeft);
	if (left == string::npos)
		return "";
	size_t right = s.find(strRight, left + strLeft.length());
	if (right == string::npos)
		return "";
	//cout << left << " " << right << "\n";
	string res = s.substr(left + strLeft.length(), right - left - strLeft.length());
	s.erase(left, right - left);
	return res;
}

inline int GetIndexInVector(int n, vector<int>& vecn)
{//get index of n in vecn
	for (int i = 0; i < vecn.size(); i++)
	{
		if (vecn[i] == n)
			return i;
	}
	return -1;
}

inline int GetFromArray(vector<int>& vecMem, int nArray, int nArraySize = 256)
{//get empty row in nArray
	int nRow = nArray * nArraySize;
	int nBack = (nArray + 1) * nArraySize;
	for (; nRow < nBack; nRow++)
	{
		if (vecMem[nRow] == 0)
			break;
	}
	if (nRow == nBack)
		nRow = -1;
	return nRow;
}

inline bool IsInVector(int n, vector<int>& vecn)
{//whether n is in vecn
	for (int i = 0; i < vecn.size(); i++)
	{
		if (vecn[i] == n)
			return true;
	}
	return false;
}

inline void RemapVector(vector<int>& vecN, std::map<int, int> mapOldNew, bool bSkip = false)
{//remap vecN with mapOldNew
	for (int i = 0; i < vecN.size(); i++)
	{
		if (mapOldNew.find(vecN[i]) == mapOldNew.end())
		{
			if (bSkip)
				vecN[i] = -100;
			continue;
		}
		vecN[i] = mapOldNew[vecN[i]];
	}
	if (bSkip)
		vecN.erase(remove(vecN.begin(), vecN.end(), -100), vecN.end());
}

inline float xmg_depth_rewrite(xmg_network& xmg, bool allow_size_increase = false, char strat = '0', float overhead = 1.0)
{
	depth_view xmg_depth{ xmg };

	xmg_algebraic_depth_rewriting_params ps;
	cut_rewriting_stats st;
	if (strat == 's')
		ps.strategy = ps.selective;
	else if (strat == 'a')
		ps.strategy = ps.aggressive;
	else
		ps.strategy = ps.dfs;
	ps.overhead = overhead;
	ps.allow_area_increase = allow_size_increase;

	xmg_algebraic_depth_rewriting(xmg_depth, ps);

	xmg = xmg_depth;

	xmg = cleanup_dangling(xmg);
	return to_seconds(st.time_total);
}

inline void xmg_resub(xmg_network& xmg, int max_insert = 2)
{
	depth_view xmg_depth{ xmg };
	fanout_view xmg_fanout{ xmg_depth };
	resubstitution_params ps;
	ps.max_inserts = max_insert;
	xmg_resubstitution(xmg_fanout, ps);
	xmg = xmg_fanout;
	xmg = cleanup_dangling(xmg);
}

inline float xmg_node_resynthesis(xmg_network& xmg, int cut_size = 4)
{
	mapping_view<xmg_network, true> mapped_xmg{ xmg };
	lut_mapping_params ps;
	node_resynthesis_stats st;
	ps.cut_enumeration_ps.cut_size = cut_size;
	lut_mapping<mapping_view<xmg_network, true>, true>(mapped_xmg, ps);

	const auto klut = *collapse_mapped_network<klut_network>(mapped_xmg);
	xmg_npn_resynthesis resyn;
	xmg = node_resynthesis<xmg_network>(klut, resyn);
	xmg = cleanup_dangling(xmg);
	return to_seconds(st.time_total);
}

inline float xmg_cut_rewrite(xmg_network& xmg, int cut_size = 4, bool bZero = true)
{
	// [06-MOD] exact_xmg_resynthesis 依赖 percy/nauty，后者在当前 MSVC
	// 与老 IMCCompiler 的全局类型冲突。先禁用这个随机分支，保证 IMC
	// 子网调度与 e-graph 集成能独立验证；后续再恢复为兼容的 exact 后端。
	(void)xmg; (void)cut_size; (void)bZero;
	return 0.0f;
}

inline void Balance(xmg_network& xmg)
{
	xmg_network n = xmg.clone();
	sop_rebalancing<xmg_network> balance_fn;
	n = balancing(n, { balance_fn });
	if (n.num_gates() <= xmg.num_gates())
		xmg = n.clone();
}

inline void Rewrite(xmg_network& xmg, bool bZero = false)
{
	// [06-MOD] 同上，保留接口以避免改动调用点；本阶段为显式 no-op。
	(void)xmg; (void)bZero;
}

inline void Refactor(xmg_network& xmg, bool bZero = false)
{
	xmg_network n = xmg.clone();
	akers_resynthesis<xmg_network> resyn;
	refactoring_params ps;
	ps.allow_zero_gain = bZero;
	refactoring(n, resyn, ps);
	n = cleanup_dangling(n);
	if (n.num_gates() <= xmg.num_gates())
		xmg = n.clone();
}

inline void Resyn2(xmg_network& xmg)
{
	Balance(xmg);
	//cout << xmg.num_gates() << " ";
	Rewrite(xmg);
	//cout << xmg.num_gates() << " ";
	Refactor(xmg);
	//cout << xmg.num_gates() << " ";
	Balance(xmg);
	//cout << xmg.num_gates() << " ";
	Rewrite(xmg);
	//cout << xmg.num_gates() << " ";
	Rewrite(xmg, true);
	//cout << xmg.num_gates() << " ";
	Balance(xmg);
	//cout << xmg.num_gates() << " ";
	Refactor(xmg, true);
	//cout << xmg.num_gates() << " ";
	Rewrite(xmg, true);
	//cout << xmg.num_gates() << " ";
	Balance(xmg);
	//cout << xmg.num_gates() << "\n";
}
