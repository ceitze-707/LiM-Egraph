#include "Scheduler.h"
#include "EgraphSubnetOptimizer.h"

Scheduler::Scheduler()
{
	m_nBound = 0;
	m_nThread = 48;
	// [06-ADD][reproducibility] 即使 rand() seed 固定，48 个工作线程的
	// 抢占顺序仍会改变子网调度结果。正式 A/B 实验可设置 IMC_THREADS=1
	// 获得确定性；默认 48 保留 baseline 的吞吐行为。
	if (const char* env_threads = std::getenv("IMC_THREADS"))
	{
		int requested_threads = std::atoi(env_threads);
		if (requested_threads > 0)
			m_nThread = requested_threads;
	}
	// 仅固定 rand() 种子并不能保证结果相同：并行 SMT/重代入中，先完成的
	// 线程会先写入共享候选。确定性模式把这类竞争全部变成固定的索引顺序。
	const char* deterministic_env = std::getenv("IMC_DETERMINISTIC");
	m_bDeterministic = deterministic_env != nullptr && *deterministic_env != '\0' && *deterministic_env != '0';
	if (m_bDeterministic)
		m_nThread = 1;
	m_nGraphBound = 80;
	// [06-ADD] baseline 的每次 ILP 分区允许 3 分钟；e-graph 候选可设置
	// 更短时限，避免一个候选耗尽整轮 DSE。
	m_nILPTimeLimitSec = 180;
	m_nSMTTimeLimitMs = 5 * 60 * 1000;
	m_nScheduleTimeLimitMs = INFINITE;
	m_epsilon = 0.1;
	m_nMFLow = 0;
	m_bStop = false;
	m_dPO = 2;

	m_increase = 0.02;
	m_critical = 0.4;
	m_nRun = 30;
	// [06-ADD] 默认仍与 baseline 一样运行 30 轮；测试时可设置 IMC_RUNS
	// （例如 2）快速验证“子网 e-graph → 全局 Pareto”这条路径。
	if (const char* env_runs = std::getenv("IMC_RUNS"))
	{
		int requested_runs = std::atoi(env_runs);
		if (requested_runs > 0)
			m_nRun = requested_runs;
	}
}

int Scheduler::CallSMT(NetList& netlist, int nMF)
{//call SMT for sub-netlist
	vector<Node>& vecNode = netlist.m_vecNode;
	vector<Node>& vecIn = netlist.m_vecIn;
	unsigned int nNumNode = vecNode.size();
	unsigned int nTotNumNode = nNumNode + vecIn.size();
	z3::context c;
	z3::optimize s(c);

	// Z3 的 SAT 核也会在内部并行；实验模式必须同时固定它，才能避免模型
	// 搜索的竞争顺序重新引入不可复现性。
	z3::set_param("sat.threads", m_bDeterministic ? 1 : 2);
	z3::params p(c);
	p.set("timeout", static_cast<unsigned int>(m_nSMTTimeLimitMs));
	s.set(p);
	vector<z3::expr_vector> matT;
	for (int n = 0; n < nNumNode; n++) {
		z3::expr_vector tmp(c);
		for (int t = 0; t < nNumNode + 1; t++)
			tmp.push_back(c.bool_const(("T_" + to_string(n) + "," + to_string(t)).c_str()));
		matT.push_back(tmp);
	}

	vector<z3::expr_vector> matA;
	for (int n = 0; n < nTotNumNode; n++) {
		z3::expr_vector tmp(c);
		//tmp.resize(nNumNode + 1);
		matA.push_back(tmp);
	}

	for (unsigned int n = 0; n < nNumNode; n++)
	{
		int nInCone = 0;
		int nOutCone = 0;
		for (int t = 0; t < nInCone + 1; t++)
			s.add(!matT[n][t]);
		for (unsigned int t = nNumNode; t > (nNumNode - 1 - nOutCone); t--)
			s.add(matT[n][t]);
		for (unsigned int t = nInCone + 1; t < (nNumNode - nOutCone - 1); t++)
			s.add(!matT[n][t] || matT[n][t + 1]);
	}

	for (unsigned t = 0; t < nNumNode; t++)
	{
		for (unsigned int i = 1; i < nNumNode; i++)
		{
			for (unsigned int j = 0; j < i; j++)
				s.add(matT[i][t] || !matT[i][t + 1] || matT[j][t] || !matT[j][t + 1]);
		}
	}

	for (Node& Nodei : vecNode)
	{
		unsigned int i = Nodei.m_nIndex;
		for (int j : Nodei.m_vecnSucc)
		{
			for (unsigned t = 0; t < nNumNode; t++)
				s.add(matT[i][t] || !matT[j][t + 1]);
		}
		if (Nodei.m_bPO)
		{
			for (unsigned int t = 0; t < nNumNode + 1; t++)
				matA[i].push_back(matT[i][t]);
		}
		else
		{
			for (unsigned int t = 0; t < nNumNode + 1; t++)
			{
				z3::expr_vector temp(c);
				for (int j : vecNode[i].m_vecnSucc)
					temp.push_back(!matT[j][t]);
				z3::expr tp = mk_or(temp);
				matA[i].push_back((tp && matT[i][t]));
			}
		}
	}

	for (Node& Nodei : vecIn)
	{
		unsigned int i = Nodei.m_nIndex;
		if (Nodei.m_bPO)
		{
			for (unsigned int t = 0; t < nNumNode + 1; t++)
				matA[i].push_back(c.bool_val(true));
		}
		else
		{
			for (unsigned int t = 0; t < nNumNode + 1; t++)
			{
				z3::expr_vector temp(c);
				for (int j : Nodei.m_vecnSucc)
					temp.push_back(!matT[j][t]);
				matA[i].push_back(mk_or(temp));
			}
		}
	}
	z3::expr maxR = c.int_val(0);
	z3::expr_vector vecActInT(c);
	for (unsigned int t = 0; t < nNumNode; t++)
	{
		z3::expr_vector temp(c);
		for (unsigned int n = 0; n < nTotNumNode; n++)
			temp.push_back(matA[n][t + 1]);
		vecActInT.push_back(sum(temp));
	}

	if (nMF == 0)
	{
		for (unsigned int t = 0; t < nNumNode; t++)
			maxR = max(vecActInT[t], maxR);
		s.minimize(maxR);

	}
	else
	{
		for (unsigned int t = 0; t < nNumNode; t++)
			s.add(vecActInT[t] <= nMF);
	}
	int nRet = -1;
	//m_netlist.m_vecnSchedule.clear();
	if (netlist.m_vecnSchedule.empty())
		netlist.m_vecnSchedule.assign(nNumNode, 0);
	/*
	if (nMF == 0)
		cout << "Running minimize mode\n";
	else
		cout << "Runnning feasibility mode, " << "Checking for " << nMF << " rows\n";

	clock_t start = clock();
	if (s.check() != z3::sat)
		cout << "Infeasible for " << nMF << " rows\n";
	else
	*/
	if (s.check() == z3::sat)
	{
		z3::model m = s.get_model();
		for (int n = 0; n < nNumNode; n++)
		{
			int nSched = nNumNode - m.eval(sum(matT[n])).get_numeral_int();
			//cout << "S" << n << ": " << nSched << "\n";
			netlist.m_vecnSchedule[nSched] = n;
		}
		for (unsigned int t = 0; t < nNumNode; t++)
			nRet = max(m.eval(vecActInT[t]).get_numeral_int(), nRet);
		//cout << "Feasible for " << nRet << " rows\n";
	}
	//cout << "Use: " << (clock() - start) * 1000 / double(CLOCKS_PER_SEC) << " ms\n";
	return nRet;
}

int Scheduler::BiSMT(NetList& netlist, int nUpper, int nLower)
{//binary search with SMT for sub-netlist
	nLower = netlist.m_vecnPO.size();//tmp
	int nRes = CallSMT(netlist, nLower);
	if (nRes != -1)
		return nRes;

	nRes = CallSMT(netlist, nUpper);
	if (nRes == -1)
		return -1;
	nUpper = nRes;

	while ((nUpper - nLower) > 1)
	{
		if (m_bStop)
			break;
		if (m_nMFLow >= nUpper)//tmp
			break;

		int nPres = round((nUpper + nLower) / 2.0);
		nRes = CallSMT(netlist, nPres);
		if (nRes == -1)
			nLower = nPres;
	//nLower = max(nPres, m_nMFLow - 1);//tmp
		else
			nUpper = nRes;
	}
	return nUpper;
}

int Scheduler::PartitionILP(NetList& netlist, double dPOWeight)
{//ILP-based partition
	vector<Node>& vecNode = netlist.m_vecNode;
	vector<Node>& vecIn = netlist.m_vecIn;
	unsigned int nNumNode = (unsigned int)vecNode.size();
	// [06-ADD][diagnostic] 仅在设置 IMC_TRACE_PARTITION 时输出未缓冲阶段标记，
	// 用于定位第三方求解器建模期间的崩溃；正常实验不产生这些诊断输出。
	const bool trace_partition = std::getenv("IMC_TRACE_PARTITION") != nullptr;
	auto trace = [&](const char* stage) {
		if (trace_partition)
			cerr << "[06 trace PartitionILP] " << stage << "\n" << flush;
	};
	trace("enter");
	cout << "Partitioning " << nNumNode << " nodes with weight = " << dPOWeight << "\n";;

	unsigned int nIn = (unsigned int)vecIn.size();
	GRBEnv env = GRBEnv(true);
	env.set("LogFile", "mip1.log");
	// [06-FIX] Gurobi 的许可证/环境初始化可能抛出 GRBException。baseline 未捕获
	// 该异常，进程会在尚未生成任何结果前终止；将其转换为本候选的失败并输出原因。
	try
	{
		env.start();
	}
	catch (const GRBException& error)
	{
		cerr << "[06 Gurobi] environment start failed: " << error.getMessage() << "\n";
		return -1;
	}
	trace("gurobi-environment-started");
	GRBModel model = GRBModel(env);
	trace("gurobi-model-created");
	model.set(GRB_IntParam_OutputFlag, 0);
	// Gurobi 的默认并行 branch-and-bound 同样可能选择不同的等价最优分区。
	// 因而 IMC_DETERMINISTIC 下显式固定为单线程、固定 solver seed。
	if (m_bDeterministic)
	{
		model.set(GRB_IntParam_Threads, 1);
		model.set(GRB_IntParam_Seed, 1);
	}
	//model.set(GRB_DoubleParam_MIPGap, 0.1);
	model.set(GRB_DoubleParam_TimeLimit, m_nILPTimeLimitSec);
	//model.set(GRB_IntParam_Threads, m_nThread);
	GRBVar* vecP = new GRBVar[nNumNode];
	GRBVar* vecSucc = new GRBVar[nNumNode + nIn];
	trace("variable-arrays-allocated");
	for (unsigned int n = 0; n < nNumNode; n++)
	{
		vecP[n] = model.addVar(0.0, 1.0, 0.0, GRB_BINARY, "P_" + to_string(n));
		vecSucc[n] = model.addVar(0.0, 1.0, 0.0, GRB_BINARY, "Succ_" + to_string(n));
		//if (n >= nNumNode)
			//model.addConstr(vecP[n] == 0);
	}
	for (unsigned int n = nNumNode; n < nNumNode + nIn; n++)
	{
		vecSucc[n] = model.addVar(0.0, 1.0, 0.0, GRB_BINARY, "Succ_" + to_string(n));
	}
	trace("variables-added");

	for (Node& Nodei : vecNode)
	{
		unsigned int i = Nodei.m_nIndex;
		vector<GRBVar> temp;
		for (int j : Nodei.m_vecnSucc)
		{
			model.addConstr(vecP[i] <= vecP[j]);
			temp.push_back(vecP[j]);
		}
		if (Nodei.m_bPO)
			model.addConstr(vecSucc[i] == 1);
		// [06-FIX] 对非 PO 且无后继的悬空内部节点，逻辑上的 OR() 为 false。
		// baseline 把 temp.data()（空指针）传入 Gurobi 的 addGenConstrOr，
		// 属于未定义的库接口用法，可能触发运行时栈保护终止。
		else if (temp.empty())
			model.addConstr(vecSucc[i] == 0);
		else
			model.addGenConstrOr(vecSucc[i], temp.data(), (int)temp.size());
	}
	trace("gate-successor-constraints-added");
	for (Node& Nodei : vecIn)
	{
		unsigned int i = Nodei.m_nIndex;
		if (Nodei.m_bPO)
			model.addConstr(vecSucc[i] == 1);
		else
		{
			vector<GRBVar> temp;
			for (int j : Nodei.m_vecnSucc)
				temp.push_back(vecP[j]);
			// [06-FIX] 输入节点同理：没有使用者且并非 PO 时，其存活标志为 false；
			// 不能向 Gurobi 传入空 OR 的数据指针。
			if (temp.empty())
				model.addConstr(vecSucc[i] == 0);
			else
				model.addGenConstrOr(vecSucc[i], temp.data(), (int)temp.size());
		}
	}
	trace("input-successor-constraints-added");

	GRBLinExpr expr = 0;
	for (unsigned int n = 0; n < nNumNode; n++)
		expr += vecP[n];
	model.addConstr(expr, GRB_LESS_EQUAL, (1 + m_epsilon) * (double)nNumNode / 2.0);
	model.addConstr(expr, GRB_GREATER_EQUAL, (1 - m_epsilon) * (double)nNumNode / 2.0);
	GRBQuadExpr qexpr = 0;
	for (unsigned int n = 0; n < nNumNode; n++)
	{
		//if (vecNode[n].m_bPO && (vecNode[n].m_vecnSucc.size() == 0))
		if (vecNode[n].m_bPO)
			qexpr += (1 - vecP[n]) * vecSucc[n] * std::max(1.0, dPOWeight);
		else
			qexpr += (1 - vecP[n]) * vecSucc[n];
	}
	for (unsigned int n = nNumNode; n < nNumNode + nIn; n++)
		qexpr += vecSucc[n];

	model.setObjective(qexpr, GRB_MINIMIZE);
	trace("objective-created");

	int nRet = -1;
	int nSecondHalf = 0;
	int nFront = 0;
	clock_t start = clock();
	trace("optimize-begin");
	model.optimize();
	trace("optimize-end");
	const int status = model.get(GRB_IntAttr_Status);
	const int solution_count = model.get(GRB_IntAttr_SolCount);
	// [06-FIX] 原实现仅识别 INFEASIBLE，却会在 TIME_LIMIT 且没有可行解
	// 时仍读取 X 属性；这会让下游使用未初始化 partition 并可能无限递归。
	if (status == GRB_INFEASIBLE || solution_count == 0)
	{
		cout << "Partition has no feasible solution (status=" << status << ")\n";
	}
	else
	{
		for (unsigned int n = 0; n < nNumNode; n++)
		{
			//cout << vecP[n].get(GRB_StringAttr_VarName) << " "
				//<< (unsigned int)round(vecP[n].get(GRB_DoubleAttr_X)) << "\n";
			if (round(vecP[n].get(GRB_DoubleAttr_X)) == 1)
			{
				vecNode[n].m_nPartition = 1;
				nSecondHalf++;
			}
			else if (round(vecSucc[n].get(GRB_DoubleAttr_X)) == 0)
				vecNode[n].m_nPartition = 0;
			else
			{
				vecNode[n].m_nPartition = 2;
				nFront++;
			}
		}
		for (unsigned int n = 0; n < nIn; n++)
		{
			if (round(vecSucc[n + nNumNode].get(GRB_DoubleAttr_X)) == 0)
				vecIn[n].m_nPartition = 0;
			else
			{
				vecIn[n].m_nPartition = 2;
				nFront++;
			}
		}
		cout << "Partitioned into: " << nNumNode - nSecondHalf << " : " << nSecondHalf << "\n";
		cout << "#Frontier=" << nFront << "\n";
		nRet = nFront;
	}
	cout << "Use: " << double(clock() - start) * 1000 / CLOCKS_PER_SEC << " ms\n";

	delete[] vecP;
	delete[] vecSucc;
	return nRet;
}

void Scheduler::IterPart(NetList& netlist, double dLeft, double dRight)
{//iterative partition
	vector<Node>& vecNode = netlist.m_vecNode;
	vector<Node>& vecIn = netlist.m_vecIn;
	vector<int>& vecnPo = netlist.m_vecnPO;
	if (vecnPo.size() > m_nMFLow)
		m_nMFLow = vecnPo.size();
	if (vecnPo.size() > m_nBound)
		m_bStop = true;
	if (m_bStop)
		return;
	int nTotSize = vecNode.size();
	if (nTotSize <= m_nGraphBound)
	{
		m_vecPartNet.push_back(netlist);
		return;
	}
	//double dPOWeight = sqrt(dLeft * dRight);
	double dPOWeight = ((double)dLeft - 1.0) / 2 + 1;
	//double dPOWeight = sqrt(dLeft);
	//double dPOWeight = std::max(1.0, 0.9 * dLeft);
	if (PartitionILP(netlist, dLeft) < 0)
	{
		// [06-FIX] ILP 无可行解/超时时立即停止这个候选，不能继续按照
		// 未定义的 partition 递归拆分。
		m_bStop = true;
		return;
	}

	NetList netlist0, netlist1;
	vector<Node>& vecNode0 = netlist0.m_vecNode;
	vector<Node>& vecNode1 = netlist1.m_vecNode;
	vector<Node>& vecIn0 = netlist0.m_vecIn;
	vector<Node>& vecIn1 = netlist1.m_vecIn;
	vector<int>& vecnNewPo0 = netlist0.m_vecnPO;
	vector<int>& vecnNewPo1 = netlist1.m_vecnPO;

	vector<int> vnPart0;
	vector<int> vnPart1;
	vector<int> vnFront;
	for (unsigned int n = 0; n < vecNode.size(); n++)
	{
		int nPart = vecNode[n].m_nPartition;
		if (nPart == 0)
			vnPart0.push_back(n);
		else if (nPart == 1)
			vnPart1.push_back(n);
		else
		{
			vnPart0.push_back(n);
			vnFront.push_back(n);
		}
	}
	sort(vnPart0.begin(), vnPart0.end());
	sort(vnPart1.begin(), vnPart1.end());
	sort(vnFront.begin(), vnFront.end());
	std::map<int, int> map0OldNew;
	std::map<int, int> map1OldNew;
	std::map<int, int> mapFOldNew;
	int nPart0Size = vnPart0.size();
	int nPart1Size = vnPart1.size();
	int nFrontier = vnFront.size();
	for (unsigned int n = 0; n < nPart0Size; n++)
		map0OldNew[vnPart0[n]] = n;
	for (unsigned int n = 0; n < nPart1Size; n++)
		map1OldNew[vnPart1[n]] = n;
	for (unsigned int n = 0; n < nFrontier; n++)
		mapFOldNew[vnFront[n]] = n;

	vector<Node> tmp0(nPart0Size);
	vecNode0 = tmp0;
	for (unsigned int i = 0; i < nPart0Size; i++)
	{
		vecNode0[i].m_nIndex = i;
		vecNode0[i].m_nOrigIndex = vecNode[vnPart0[i]].m_nOrigIndex;
		for (int jold : vecNode[vnPart0[i]].m_vecnSucc)
		{
			if (map0OldNew.find(jold) != map0OldNew.end())
				vecNode0[i].m_vecnSucc.push_back(map0OldNew[jold]);
		}
	}

	vecnNewPo0.clear();
	for (int front : vnFront)
	{
		vecnNewPo0.push_back(map0OldNew[front]);
		vecNode0[map0OldNew[front]].m_bPO = true;
	}

	vector<Node> tmp0i(vecIn.size());
	vecIn0 = tmp0i;
	for (unsigned int i = 0; i < vecIn.size(); i++)
	{
		vecIn0[i].m_nIndex = nPart0Size + i;
		vecIn0[i].m_nOrigIndex = vecIn[i].m_nOrigIndex;
		for (int jold : vecIn[i].m_vecnSucc)
		{
			if (map0OldNew.find(jold) != map0OldNew.end())
				vecIn0[i].m_vecnSucc.push_back(map0OldNew[jold]);
		}
		if (vecIn[i].m_nPartition == 2)
		{
			vecnNewPo0.push_back(nPart0Size + i);
			vecIn0[i].m_bPO = true;
		}
	}
	//IterPart(netlist0, dLeft, dPOWeight);
	IterPart(netlist0, dPOWeight, dRight);

	vector<Node> tmp1(nPart1Size);
	vecNode1 = tmp1;
	for (unsigned int i = 0; i < nPart1Size; i++)
	{
		vecNode1[i].m_nIndex = i;
		vecNode1[i].m_nOrigIndex = vecNode[vnPart1[i]].m_nOrigIndex;
		for (int jold : vecNode[vnPart1[i]].m_vecnSucc)
		{
			if (map1OldNew.find(jold) != map1OldNew.end())
				vecNode1[i].m_vecnSucc.push_back(map1OldNew[jold]);
		}
	}
	vector<int> vecnInFront;
	for (unsigned int i = 0; i < vecIn.size(); i++)
	{
		if (vecIn[i].m_nPartition == 2)
			vecnInFront.push_back(vecIn[i].m_nIndex);
	}
	int nInFront = vecnInFront.size();

	vector<Node> tmp1i(nFrontier + nInFront);
	vecIn1 = tmp1i;
	for (unsigned int i = 0; i < nFrontier; i++)
	{
		vecIn1[i].m_nIndex = nPart1Size + i;
		vecIn1[i].m_nOrigIndex = vecNode[vnFront[i]].m_nOrigIndex;
		for (int jold : vecNode[vnFront[i]].m_vecnSucc)
		{
			if (map1OldNew.find(jold) != map1OldNew.end())
				vecIn1[i].m_vecnSucc.push_back(map1OldNew[jold]);
		}
	}

	vecnNewPo1.clear();
	for (int po : vecnPo)
	{
		if (map1OldNew.find(po) != map1OldNew.end())
		{
			vecnNewPo1.push_back(map1OldNew[po]);
			vecNode1[map1OldNew[po]].m_bPO = true;
		}
		else if (mapFOldNew.find(po) != mapFOldNew.end())
		{
			vecnNewPo1.push_back(mapFOldNew[po] + nPart1Size);
			vecIn1[mapFOldNew[po]].m_bPO = true;
		}
	}
	for (unsigned int i = 0; i < nInFront; i++)
	{
		vecIn1[i + nFrontier].m_nIndex = nPart1Size + nFrontier + i;
		vecIn1[i + nFrontier].m_nOrigIndex = vecIn[vecnInFront[i] - nTotSize].m_nOrigIndex;
		for (int jold : vecIn[vecnInFront[i] - nTotSize].m_vecnSucc)
		{
			if (map1OldNew.find(jold) != map1OldNew.end())
				vecIn1[i + nFrontier].m_vecnSucc.push_back(map1OldNew[jold]);
		}
		if (vecIn[vecnInFront[i] - nTotSize].m_bPO)
		{
			vecnNewPo1.push_back(nPart1Size + nFrontier + i);
			vecIn1[i + nFrontier].m_bPO = true;
		}
	}
	IterPart(netlist1, dPOWeight, dRight);
}

void Scheduler::ScheduleThread()
{//scheduling thread
	while (WaitForSingleObject(m_hEventKillThread, 1) != WAIT_OBJECT_0)
	{
		if (m_bStop)
			break;
		LONG nID = InterlockedIncrement(&m_nProcNet);
		if (nID > m_nTotNet)
			break;
		int nSch = BiSMT(m_vecPartNet[nID - 1], m_nBound, m_nMFLow);
		if (nSch == -1)
		{
			m_bStop = true;
			break;
		}
		EnterCriticalSection(&m_rCritical);
		if (m_nMFLow < nSch)
			m_nMFLow = nSch;
		m_nProgressDone += m_vecPartNet[nID - 1].m_vecNode.size();
		LeaveCriticalSection(&m_rCritical);
		cout << "=====================Progress: scheduled " << m_nProgressDone << "/" << m_nProgressTotal << " nodes. Sub MF: "
			<< nSch << "\n";
	}
}

DWORD WINAPI MyThreadFunction(LPVOID lpParam)
{
	if (lpParam == NULL)
		return 0;

	Scheduler* pMyScheduler = (Scheduler*)lpParam;
	pMyScheduler->ScheduleThread();
	return 0;
}

void Scheduler::ThreadIterPartScheduler()
{//multi-thread interative partition scheduler
	cout << "Scheduling netlist with " << m_netlist.m_vecNode.size() << " nodes with MF bound " << m_nBound << "\n";
	m_bStop = false;
	m_nProgressTotal = m_netlist.m_vecNode.size();
	m_nProgressDone = 0;
	m_nMFLow = 0;
	m_vecPartNet.clear();
	clock_t start = clock();
	IterPart(m_netlist, m_dPO, 1);
	if (m_bStop)
	{
		cout << "MF after partition too large\n";
		return;
	}
	m_nTotNet = m_vecPartNet.size();
	m_nProcNet = 0;
	cout << "Partition Run time: " << double(clock() - start) * 1000 / CLOCKS_PER_SEC << " ms\n";
	cout << "MF lower bound: " << m_nMFLow << "\n";

	if (m_bDeterministic)
	{
		// 固定子网序号 0,1,... 的调度和 MF 下界传播顺序；不创建 Windows
		// 工作线程，因此每次相同输入都会经历相同的 BiSMT 调用序列。
		cout << "Deterministic schedule: serial subnet order\n";
		for (int nID = 0; nID < m_nTotNet; ++nID)
		{
			int nSch = BiSMT(m_vecPartNet[nID], m_nBound, m_nMFLow);
			if (nSch == -1)
			{
				m_bStop = true;
				break;
			}
			m_nMFLow = max(m_nMFLow, nSch);
			m_nProgressDone += m_vecPartNet[nID].m_vecNode.size();
			cout << "=====================Progress: scheduled " << m_nProgressDone << "/" << m_nProgressTotal
				<< " nodes. Sub MF: " << nSch << "\n";
		}
	}
	else
	{
		InitializeCriticalSection(&m_rCritical);
		m_hEventKillThread = CreateEvent(NULL, TRUE, FALSE, NULL); // manual reset, initially reset
		HANDLE* pThreadHandle = new HANDLE[m_nThread];
		PDWORD pdwThreadID = new DWORD[m_nThread];
		for (int i = 0; i < m_nThread; i++)
		{
			pThreadHandle[i] = CreateThread(NULL, 0, MyThreadFunction, this, 0, pdwThreadID + i);
			assert(pThreadHandle[i] != NULL);
		}
		const DWORD wait_result = WaitForMultipleObjects(
			m_nThread, pThreadHandle, TRUE, m_nScheduleTimeLimitMs);
		if (wait_result == WAIT_TIMEOUT)
		{
			// [06-FIX] 额外 e-graph 候选不能让 48 个 SMT 工作线程无限占用
			// DSE。通知线程停止，再等待正在执行的有界 SMT 调用退出。
			cout << "Schedule timed out; skip this candidate\n";
			m_bStop = true;
			SetEvent(m_hEventKillThread);
			WaitForMultipleObjects(m_nThread, pThreadHandle, TRUE, INFINITE);
		}
		for (int i = 0; i < m_nThread; i++)
			CloseHandle(pThreadHandle[i]);

		delete[] pThreadHandle;
		delete[] pdwThreadID;
		CloseHandle(m_hEventKillThread);
		DeleteCriticalSection(&m_rCritical);
	}

	for (NetList& net : m_vecPartNet)
	{
		for (int n : net.m_vecnSchedule)
			m_netlist.m_vecnSchedule.push_back(net.m_vecNode[n].m_nOrigIndex);
	}
	m_nFootPrint = m_nMFLow;
	if (m_nFootPrint < m_netlist.m_vecIn.size())
		m_nFootPrint = m_netlist.m_vecIn.size();
	cout << "Schedule ends, MF = " << m_nFootPrint << "\n";
}

bool Scheduler::IsDivisor(const Node& ndRoot, const Node& ndDiv)
{//whether ndDiv can be divisor for ndRoot
	const std::set<int>& setRootConePI = ndRoot.m_setnConePI; // [06-MOD] 同上。
	int nNotIn = 0;
	for (int pi : ndDiv.m_setnConePI)
	{
		if (setRootConePI.find(pi) == setRootConePI.end())
			nNotIn++;
	}
	if (nNotIn < 2)
		return true;
	return false;
}

void Scheduler::ResubThread()
{//thread for MF-Resub
	while (WaitForSingleObject(m_hEventKillThread, 1) != WAIT_OBJECT_0)
	{
		if (m_bFoundResub)
			break;
		LONG m = InterlockedIncrement(&m_nCheckPeak);
		if (m >= m_vecPeakIndex.size())
			break;
		clock_t start = clock();
		NetList netlist = m_curNetlist;
		netlist.m_net = m_curNetlist.m_net.clone();
		int nPeakEnd = m_vecPeakIndex.back();
		Node& nd = netlist.m_vecNode[m_vecPeakIndex[m]];
		if (nd.m_bPO)
			continue;
		int nFanOut = -1;
		for (int fo : nd.m_vecnSucc)
		{
			if (fo > nPeakEnd)
			{
				if (nFanOut != -1)
				{
					nFanOut = -1;
					break;
				}
				nFanOut = fo;
			}
		}
		if (nFanOut == -1)
			continue;
		int foID = nFanOut + netlist.m_nOffset;
		int mID = m_vecPeakIndex[m] + netlist.m_nOffset;
		//cout << "node " << mID << " has single FO " << foID << "\n";
		vector<int> vecCand;
		for (int p : m_vecPeakIndex)
		{
			if (p != m_vecPeakIndex[m] && IsDivisor(netlist.m_vecNode[nFanOut], netlist.m_vecNode[p]))
				vecCand.push_back(p + netlist.m_nOffset);
		}
		for (int i = nPeakEnd + 1; i < nFanOut; i++)
		{
			if (IsDivisor(netlist.m_vecNode[nFanOut], netlist.m_vecNode[i]))
				vecCand.push_back(i + netlist.m_nOffset);
		}

		for (int pi : netlist.m_vecNode[nFanOut].m_setnConePI)
			vecCand.push_back(pi + 1);
		vecCand.push_back(0);

		int nCandSize = vecCand.size();
		//cout << "#candidate nodes: " << nCandSize << "\n";
		if (nCandSize < 3)
			continue;
		InterlockedIncrement(&m_nRoot);
		auto tt = m_tts[foID];
		bool bRand = true;
		//int nTrial = 150000;
		//if (nCandSize <= 100)
		int nTrial = 1500000;
		if (nCandSize <= 200)
		{
			bRand = false;
			nTrial = nCandSize * nCandSize * nCandSize;
		}
		for (int t = 0; t < nTrial; t++)
		{
			if (m_bFoundResub)
				break;
			int xID, yID, zID;
			if (bRand)
			{
				xID = vecCand[rand() % nCandSize];
				while (true)
				{
					yID = vecCand[rand() % nCandSize];
					if (yID != xID)
						break;
				}
				while (true)
				{
					zID = vecCand[rand() % nCandSize];
					if (zID != xID && zID != yID)
						break;
				}

			}
			else
			{
				xID = vecCand[t % nCandSize];
				yID = vecCand[(t / nCandSize) % nCandSize];
				zID = vecCand[t / nCandSize / nCandSize];
				if (xID <= yID || yID <= zID)
					continue;
			}
			validator_params vps;
			vps.max_clauses = 1000;
			vps.conflict_limit = 100;

			const kitty::partial_truth_table& ttx = m_tts[xID];
			const kitty::partial_truth_table& tty = m_tts[yID];
			const kitty::partial_truth_table& ttz = m_tts[zID];

			vector<int> vecnPredID = { xID, yID, zID };
			vector<bool> vecbPredComp = { m_vecPhase[xID] , m_vecPhase[yID] , m_vecPhase[zID] };
			if (tt == detail::ternary_xor(ttx, tty, ttz))
			{
				xmg_network ntk_s = netlist.m_net.clone();
				xmg_network::signal sx = m_vecPhase[xID] ? !ntk_s.make_signal(xID) : ntk_s.make_signal(xID);
				xmg_network::signal sy = m_vecPhase[yID] ? !ntk_s.make_signal(yID) : ntk_s.make_signal(yID);
				xmg_network::signal sz = m_vecPhase[zID] ? !ntk_s.make_signal(zID) : ntk_s.make_signal(zID);
				circuit_validator<xmg_network, bill::solvers::bsat2> validator_s(ntk_s, vps);
				//cout << "--------------------------------------Find candidate XOR " << xID << " " << yID << " " << zID << "\n";
				xmg_network::signal candidate = m_vecPhase[foID] ? !ntk_s.create_xor3(sx, sy, sz) : ntk_s.create_xor3(sx, sy, sz);
				optional<bool> res = validator_s.validate(ntk_s.make_signal(foID), candidate);
				if (!res || !(*res))
				{
					InterlockedIncrement(&m_nVFail);
					continue;
				}
				else
				{
					EnterCriticalSection(&m_rCritical);
					//cout << "----------Substitute fanins of " << foID << " into XOR " << xID << " " << yID << " " << zID << "\n";
					//cout << vecbPredComp[0] << " " << vecbPredComp[1] << " " << vecbPredComp[2] << " " << m_vecPhase[foID] << "\n";
					netlist.SubPred(nFanOut, vecnPredID, vecbPredComp, false, m_vecPhase[foID]);
					m_newNetlist = netlist;
					m_bFoundResub = true;
					LeaveCriticalSection(&m_rCritical);
					//ntk_s.substitute_node(foID, candidate);
					//write_verilog(ntk_s, "ttt.v");
					//netlist.m_net = ntk_s.clone();
					//cout << "Valid Fail: " << m_nVFail << "\n";
					break;
				}
			}
			if (tt == kitty::ternary_majority(ttx, tty, ttz))
			{
				xmg_network ntk_s = netlist.m_net.clone();
				xmg_network::signal sx = m_vecPhase[xID] ? !ntk_s.make_signal(xID) : ntk_s.make_signal(xID);
				xmg_network::signal sy = m_vecPhase[yID] ? !ntk_s.make_signal(yID) : ntk_s.make_signal(yID);
				xmg_network::signal sz = m_vecPhase[zID] ? !ntk_s.make_signal(zID) : ntk_s.make_signal(zID);
				circuit_validator<xmg_network, bill::solvers::bsat2> validator_s(ntk_s, vps);
				//cout << "--------------------------------------Find candidate MAJ " << xID << " " << yID << " " << zID << "\n";
				xmg_network::signal candidate = m_vecPhase[foID] ? !ntk_s.create_maj(sx, sy, sz) : ntk_s.create_maj(sx, sy, sz);
				optional<bool> res = validator_s.validate(ntk_s.make_signal(foID), candidate);
				if (!res || !(*res))
				{
					InterlockedIncrement(&m_nVFail);
					continue;
				}
				else
				{
					EnterCriticalSection(&m_rCritical);
					//cout << "----------Substitute fanins of " << foID << " into MAJ " << xID << " " << yID << " " << zID << "\n";
					//cout << vecbPredComp[0] << " " << vecbPredComp[1] << " " << vecbPredComp[2] << " " << m_vecPhase[foID] << "\n";
					netlist.SubPred(nFanOut, vecnPredID, vecbPredComp, true, m_vecPhase[foID]);
					m_newNetlist = netlist;
					m_bFoundResub = true;
					LeaveCriticalSection(&m_rCritical);
					//ntk_s.substitute_node(foID, candidate);
					//write_verilog(ntk_s, "ttt.v");
					//netlist.m_net = ntk_s.clone();
					//cout << "Valid Fail: " << m_nVFail << "\n";
					break;
				}
			}
			if (tt == kitty::ternary_majority(~ttx, tty, ttz))
			{
				xmg_network ntk_s = netlist.m_net.clone();
				xmg_network::signal sx = m_vecPhase[xID] ? !ntk_s.make_signal(xID) : ntk_s.make_signal(xID);
				xmg_network::signal sy = m_vecPhase[yID] ? !ntk_s.make_signal(yID) : ntk_s.make_signal(yID);
				xmg_network::signal sz = m_vecPhase[zID] ? !ntk_s.make_signal(zID) : ntk_s.make_signal(zID);
				circuit_validator<xmg_network, bill::solvers::bsat2> validator_s(ntk_s, vps);
				//cout << "--------------------------------------Find candidate NMAJ " << xID << " " << yID << " " << zID << "\n";
				xmg_network::signal candidate = m_vecPhase[foID] ? !ntk_s.create_maj(!sx, sy, sz) : ntk_s.create_maj(!sx, sy, sz);
				optional<bool> res = validator_s.validate(ntk_s.make_signal(foID), candidate);
				if (!res || !(*res))
				{
					InterlockedIncrement(&m_nVFail);
					continue;
				}
				else
				{
					EnterCriticalSection(&m_rCritical);
					//cout << "----------Substitute fanins of " << foID << " into NMAJ0 " << xID << " " << yID << " " << zID << "\n";
					vecbPredComp[0] = !vecbPredComp[0];
					//cout << vecbPredComp[0] << " " << vecbPredComp[1] << " " << vecbPredComp[2] << " " << m_vecPhase[foID] << "\n";
					netlist.SubPred(nFanOut, vecnPredID, vecbPredComp, true, m_vecPhase[foID]);
					m_newNetlist = netlist;
					m_bFoundResub = true;
					LeaveCriticalSection(&m_rCritical);
					//ntk_s.substitute_node(foID, candidate);
					//write_verilog(ntk_s, "ttt.v");
					//netlist.m_net = ntk_s.clone();
					//cout << "Valid Fail: " << m_nVFail << "\n";
					break;
				}
			}
			if (tt == kitty::ternary_majority(ttx, ~tty, ttz))
			{
				xmg_network ntk_s = netlist.m_net.clone();
				xmg_network::signal sx = m_vecPhase[xID] ? !ntk_s.make_signal(xID) : ntk_s.make_signal(xID);
				xmg_network::signal sy = m_vecPhase[yID] ? !ntk_s.make_signal(yID) : ntk_s.make_signal(yID);
				xmg_network::signal sz = m_vecPhase[zID] ? !ntk_s.make_signal(zID) : ntk_s.make_signal(zID);
				circuit_validator<xmg_network, bill::solvers::bsat2> validator_s(ntk_s, vps);
				//cout << "--------------------------------------Find candidate NMAJ " << xID << " " << yID << " " << zID << "\n";
				xmg_network::signal candidate = m_vecPhase[foID] ? !ntk_s.create_maj(sx, !sy, sz) : ntk_s.create_maj(sx, !sy, sz);
				optional<bool> res = validator_s.validate(ntk_s.make_signal(foID), candidate);
				if (!res || !(*res))
				{
					InterlockedIncrement(&m_nVFail);
					continue;
				}
				else
				{
					EnterCriticalSection(&m_rCritical);
					//cout << "----------Substitute fanins of " << foID << " into NMAJ1 " << xID << " " << yID << " " << zID << "\n";
					vecbPredComp[1] = !vecbPredComp[1];
					//cout << vecbPredComp[0] << " " << vecbPredComp[1] << " " << vecbPredComp[2] << " " << m_vecPhase[foID] << "\n";
					netlist.SubPred(nFanOut, vecnPredID, vecbPredComp, true, m_vecPhase[foID]);
					m_newNetlist = netlist;
					m_bFoundResub = true;
					LeaveCriticalSection(&m_rCritical);
					//ntk_s.substitute_node(foID, candidate);
					//write_verilog(ntk_s, "ttt.v");
					//netlist.m_net = ntk_s.clone();
					//cout << "Valid Fail: " << m_nVFail << "\n";
					break;
				}
			}
			if (tt == kitty::ternary_majority(ttx, tty, ~ttz))
			{
				xmg_network ntk_s = netlist.m_net.clone();
				xmg_network::signal sx = m_vecPhase[xID] ? !ntk_s.make_signal(xID) : ntk_s.make_signal(xID);
				xmg_network::signal sy = m_vecPhase[yID] ? !ntk_s.make_signal(yID) : ntk_s.make_signal(yID);
				xmg_network::signal sz = m_vecPhase[zID] ? !ntk_s.make_signal(zID) : ntk_s.make_signal(zID);
				circuit_validator<xmg_network, bill::solvers::bsat2> validator_s(ntk_s, vps);
				//cout << "--------------------------------------Find candidate NMAJ " << xID << " " << yID << " " << zID << "\n";
				xmg_network::signal candidate = m_vecPhase[foID] ? !ntk_s.create_maj(sx, sy, !sz) : ntk_s.create_maj(sx, sy, !sz);
				optional<bool> res = validator_s.validate(ntk_s.make_signal(foID), candidate);
				if (!res || !(*res))
				{
					InterlockedIncrement(&m_nVFail);
					continue;
				}
				else
				{
					EnterCriticalSection(&m_rCritical);
					//cout << "----------Substitute fanins of " << foID << " into NMAJ2 " << xID << " " << yID << " " << zID << "\n";
					vecbPredComp[2] = !vecbPredComp[2];
					//cout << vecbPredComp[0] << " " << vecbPredComp[1] << " " << vecbPredComp[2] << " " << m_vecPhase[foID] << "\n";
					netlist.SubPred(nFanOut, vecnPredID, vecbPredComp, true, m_vecPhase[foID]);
					m_newNetlist = netlist;
					m_bFoundResub = true;
					LeaveCriticalSection(&m_rCritical);
					//ntk_s.substitute_node(foID, candidate);
					//write_verilog(ntk_s, "ttt.v");
					//netlist.m_net = ntk_s.clone();
					//cout << "Valid Fail: " << m_nVFail << "\n";
					break;
				}
			}
		}
		//cout << mID<< " #candidate nodes: " << nCandSize << " thread time: " << double(clock() - start) / CLOCKS_PER_SEC << " s\n";
	}
}


DWORD WINAPI ThreadResub(LPVOID lpParam)
{
	if (lpParam == NULL)
		return 0;

	Scheduler* pMyScheduler = (Scheduler*)lpParam;
	pMyScheduler->ResubThread();
	return 0;
}


bool Scheduler::ResubMult(NetList& netlist)
{//multi-thread resub
	m_nVFail = 0;
	m_nRoot = 0;
	xmg_network ntk = netlist.m_net.clone();
	int nNumNode = netlist.m_vecNode.size();
	m_vecPeakIndex = netlist.GetPeak();
	int nPeakSize = m_vecPeakIndex.size();
	//cout << "peak with " << nPeakSize << " nodes\n";

	//simulate
	partial_simulator sim(ntk.num_pis(), 1000);
	pattern_generation_params ps;
	ps.odc_levels = 5;
	pattern_generation(ntk, sim, ps);
	unordered_node_map<kitty::partial_truth_table, xmg_network> tts(ntk);
	simulate_nodes<xmg_network>(ntk, tts, sim, true);
	validator_params vps;
	vps.max_clauses = 1000;
	vps.conflict_limit = 100;
	circuit_validator<xmg_network, bill::solvers::bsat2> validator(ntk, vps);
	vector<bool> vecPhase(tts.size());
	for (int n = 0; n < tts.size(); n++)
	{
		auto& tt = tts[n];
		if (kitty::get_bit(tt, 0))
		{
			tt = ~tt;
			vecPhase[n] = true;
		}
		else
			vecPhase[n] = false;
	}

	//cout << "Checking for resub in peak\n";
	for (int m = 0; m < nPeakSize; m++)
	{
		int mID = m_vecPeakIndex[m] + netlist.m_nOffset;
		/*
		if (tts[mID] == tts[0])
		{
			cout << "node " << mID << "candidate constant\n";
			xmg_network::signal candidate = vecPhase[mID] ? ntk.get_constant(true) : ntk.get_constant(false);
			optional<bool> res = validator.validate(ntk.make_signal(mID), candidate);
			if (!res || !(*res))
				cout << "checked false \n";
			else
			{
				cout << "checked true!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
				ntk.substitute_node(mID, candidate);
				return true;
			}
		}
		*/
		for (int n = m + 1; n < nPeakSize; n++)
		{
			int nID = m_vecPeakIndex[n] + netlist.m_nOffset;
			if (tts[mID] == tts[nID])
			{
				//cout << "node " << mID << " candidate equivalent to node " << nID << "\n";
				xmg_network::signal candidate = (vecPhase[mID] ^ vecPhase[nID]) ? !ntk.make_signal(nID) : ntk.make_signal(nID);
				optional<bool> res = validator.validate(ntk.make_signal(mID), candidate);
				if (!res || !(*res))
					continue;
				else
				{
					//cout << "----------Substitute node " << mID << " with " << nID << " " << (vecPhase[mID] ^ vecPhase[nID]) << "\n";
					netlist.SubNode(std::min(m_vecPeakIndex[m], m_vecPeakIndex[n]), std::max(m_vecPeakIndex[m], m_vecPeakIndex[n]), vecPhase[mID] ^ vecPhase[nID]);
					return true;
				}
			}
		}
	}

	//cout << "Checking for resub in fanout of peak\n";
	m_bFoundResub = false;
	m_curNetlist = netlist;
	m_nCheckPeak = -1;
	m_tts.clear();
	for (int n = 0; n < tts.size(); n++)
		m_tts.push_back(tts[n]);
	m_vecPhase = vecPhase;
	InitializeCriticalSection(&m_rCritical);
	m_hEventKillThread = CreateEvent(NULL, TRUE, FALSE, NULL); // manual reset, initially reset
	if (m_bDeterministic)
	{
		// ResubThread 通过 InterlockedIncrement 依序取峰值根；直接调用即可保留
		// 完整验证逻辑，同时消除“哪个线程先发现替换”的竞态。
		ResubThread();
	}
	else
	{
		HANDLE* pThreadHandle = new HANDLE[m_nThread];
		PDWORD pdwThreadID = new DWORD[m_nThread];
		for (int i = 0; i < m_nThread; i++)
		{
			pThreadHandle[i] = CreateThread(NULL, 0, ThreadResub, this, 0, pdwThreadID + i);
			assert(pThreadHandle[i] != NULL);
		}
		WaitForMultipleObjects(m_nThread, pThreadHandle, TRUE, INFINITE);
		for (int i = 0; i < m_nThread; i++)
			CloseHandle(pThreadHandle[i]);

		delete[] pThreadHandle;
		delete[] pdwThreadID;
	}
	CloseHandle(m_hEventKillThread);
	DeleteCriticalSection(&m_rCritical);

	//cout << "===== root = " << m_nRoot << ", valid fail = " << m_nVFail << "\n";
	if (m_bFoundResub)
	{
		netlist = m_newNetlist;
		return true;
	}
	//cout << "no resub\n";
	return false;
}

bool Scheduler::UpdatePareto(NetList& NewNetList, vector<NetList>& vecParetoList)
{//update pareto
	int nNewSize = NewNetList.m_nSize;
	int nNewMF = NewNetList.m_nMF;
	bool bPareto = true;
	for (auto nb = vecParetoList.begin(); nb != vecParetoList.end();) {
		const NetList& paretopt = (*nb);
		if ((paretopt.m_nSize < nNewSize && paretopt.m_nMF <= nNewMF)
			|| (paretopt.m_nSize <= nNewSize && paretopt.m_nMF < nNewMF))
		{
			bPareto = false;
			break;
		}
		if ((nNewSize < paretopt.m_nSize && nNewMF <= paretopt.m_nMF)
			|| (nNewSize <= paretopt.m_nSize && nNewMF < paretopt.m_nMF))
			nb = vecParetoList.erase(nb);
		else
			++nb;
	}
	if (bPareto)
		vecParetoList.push_back(NewNetList);
	return bPareto;
}

void Scheduler::NewDSE()
{
	clock_t start = clock();
	int nOrigOffset = m_netlist.m_nOffset;
	int nAcceptNode = m_netlist.m_vecNode.size() * (1 + m_increase);
	std::ofstream ofCSV;
	string strResultName = m_netlist.m_strBench + "_result.csv";
	ofCSV.open(strResultName.c_str(), std::ofstream::out);
	// [06-ADD][measurement] 将日志中的关键候选数据同时写成 CSV，供后续
	// 筛选器分析使用。Tag 由实验启动脚本指定，防止不同 seed 覆盖彼此数据。
	string metrics_tag = "latest";
	if (const char* configured = std::getenv("IMC_METRICS_TAG"))
		if (*configured != '\0') metrics_tag = configured;
	string strMetricsName = m_netlist.m_strBench + "_" + metrics_tag + "_candidate_metrics.csv";
	std::ofstream ofMetrics(strMetricsName.c_str(), std::ofstream::out);
	ofMetrics << "round,record_type,branch,candidates,replacements,gates_before,gates_after,"
		<< "depth_before,depth_after,fanout_before,fanout_after,mffc_total,boundary_total,"
		<< "boundary_max,local_gain_total,size,mf,cross,schedule_wall_ms,total_wall_ms,status\n";
	cout << "++++++++++++++++++++Bench: " << m_netlist.m_strBench << "++++++++++++++++++++\n";
	cout << "-----------------Round 0:-----------------\n";
	ThreadIterPartScheduler();
	NetList ReorderedNet = m_netlist.ReorderNodes();
	ReorderedNet.m_net = ReorderedNet.ConstructXMG();
	ReorderedNet.ConfigMF();
	ofCSV << ReorderedNet.m_nSize << ", " << ReorderedNet.m_nMF << "," << ReorderedNet.m_nCross << "\n";

	vector<NetList> vecParetoList;
	vecParetoList.push_back(ReorderedNet);

	clock_t beginn = clock();
	cout << "Begin resub\n";
	int nNumSubb = 0;
	NetList netlistt;
	NetList EmptyListt;
	netlistt.m_net = ReorderedNet.m_net.clone();
	netlistt.ConfigWithXMG();
	netlistt.m_vecnSchedule.clear();
	for (int i = 0; i < netlistt.m_vecNode.size(); i++)
		netlistt.m_vecnSchedule.push_back(i);
	netlistt.ConfigMF();
	//cout << netlistt.m_nSize << " " << netlistt.m_nMF << " --\n";
	xmg_network orig_nett = netlistt.m_net.clone();
	while (true)
	{
		if (!ResubMult(netlistt))
			break;
		nNumSubb++;
		//cout << "===== root = " << m_nRoot << "\n";
		//cout << "===== valid fail = " << m_nVFail << "\n";
		cout << nNumSubb << "-th resub\n";
		xmg_network new_nett = netlistt.ConstructXMG();
		netlistt = EmptyListt;
		netlistt.m_net = new_nett.clone();
		netlistt.ConfigWithXMG();
		netlistt.m_vecnSchedule.clear();
		for (int i = 0; i < netlistt.m_vecNode.size(); i++)
			netlistt.m_vecnSchedule.push_back(i);
		netlistt.ConfigMF();
		cout << "New netlist: #nodes = " << netlistt.m_nSize << ", MF = " << netlistt.m_nMF << "\n";
		ofCSV << netlistt.m_nSize << ", " << netlistt.m_nMF << "," << netlistt.m_nCross << "," << 1 << "\n";
		if (UpdatePareto(netlistt, vecParetoList))
			cout << "Into Pareto\n";
		else
			cout << "Not Pareto\n";

		if (netlistt.DelRedun())
		{
			cout << "delete reduntant---------------------\n";
			xmg_network newnew_nett = netlistt.ConstructXMG();
			netlistt = EmptyListt;
			netlistt.m_net = newnew_nett.clone();
			netlistt.ConfigWithXMG();
			netlistt.m_vecnSchedule.clear();
			for (int i = 0; i < netlistt.m_vecNode.size(); i++)
				netlistt.m_vecnSchedule.push_back(i);
			netlistt.ConfigMF();
			cout << "New netlist: #nodes = " << netlistt.m_nSize << ", MF = " << netlistt.m_nMF << "\n";
			ofCSV << netlistt.m_nSize << ", " << netlistt.m_nMF << "," << netlistt.m_nCross << "," << 1 << "\n";
			if (UpdatePareto(netlistt, vecParetoList))
				cout << "Into Pareto\n";
			else
				cout << "Not Pareto\n";
		}
		else
			cout << "no reduntant nodes--------------------\n";
	}
	cout << "End resub\n";
	cout << "Time = " << double(clock() - beginn) / CLOCKS_PER_SEC << ", #resub = " << nNumSubb << "\n";


	for (int i = 1; i < m_nRun; i++)
	{
		cout << "-----------------Round " << i << ":-----------------\n";
		int nPick = rand() % vecParetoList.size(); // 从当前非支配全网中随机选一个起点。
		// [06-MOD] A/B 比较会在候选一进入 Pareto 后修改 vecParetoList，
		// 因此必须复制被选中的设计，不能持有会失效的 vector 引用。
		NetList OrigNet = vecParetoList[nPick];
		cout << "Pick " << nPick << "-th pareto design\n";
		int nBegin = OrigNet.m_vecMaxMFIndex[0]; // 峰值活跃区间的首个节点。
		int nEnd = OrigNet.m_vecMaxMFIndex.back(); // 峰值活跃区间的末个节点。
		int nCritical = (double)OrigNet.m_nMF * m_critical; // 向两侧扩张的关键活跃度阈值。
		//cout << "Begin/end " << nBegin << "/" << nEnd << "\n";
		//cout << "critical " << nCritical << "\n";
		while (nBegin > 0)
		{
			nBegin--;
			if (OrigNet.m_vecMF[nBegin] < nCritical)
				break;
		}
		while (nEnd < OrigNet.m_vecNode.size() - 1)
		{
			nEnd++;
			if (OrigNet.m_vecMF[nEnd] < nCritical)
				break;
		}

		//cout << "Begin/end " << nBegin << "/" << nEnd << "\n";
		int nOtherNode = OrigNet.m_vecNode.size() - (nEnd - nBegin + 1); // 切口外保持不变的节点数。

		vector<int> vecTIOrigIndex; // 子网临时输入在原网中的编号。
		vector<int> vecPOOrigIndex; // 子网输出在原网中的编号。
		NetList Sub = OrigNet.ExtractSub(nBegin, nEnd, vecTIOrigIndex, vecPOOrigIndex); // 从全网抽取可独立优化的关键子网。
		xmg_network NewNet = Sub.m_net.clone(); // 原 IMC 逻辑优化分支的工作副本。
		cout << "Extract " << nEnd - nBegin + 1 << " nodes " << nBegin << " - " << nEnd << " / " << OrigNet.m_vecNode.size() << "\n";
		// [06-ADD] 仅用于集成烟雾测试：跳过原随机局部重写，让 e-graph
		// 直接面对提取子网。默认未设置该环境变量，行为与原 IMC 路径一致。
		bool bSkipLocalRewrite = (std::getenv("IMC_SKIP_LOCAL_REWRITE") != nullptr); // 测试开关：直接比较未重写子网与 e-graph。
		if (bSkipLocalRewrite)
			cout << "[06 test] skip local IMC rewrite before A/B comparison\n";
		while (!bSkipLocalRewrite) // 正常运行时反复尝试传统局部 XMG 重写。
		{
			//int nNodeMin = INT_MAX;
			//xmg_network KeepNet = Sub.m_net.clone();
			for (int j = 0; j < 10; j++) // 每次随机组合十种算子。
			{
				int nCommand = rand() % 10; // 选取本步应用的传统综合算子。
				//cout << nCommand << " ";
				if (nCommand == 0)
					xmg_depth_rewrite(NewNet, true, 'a', 1.2);
				else if (nCommand == 1)
					xmg_depth_rewrite(NewNet, false, 's');
				else if (nCommand == 2)
					xmg_resub(NewNet);
				else if (nCommand == 3)
					xmg_node_resynthesis(NewNet, 3);
				else if (nCommand == 4)
					xmg_node_resynthesis(NewNet, 4);
				else if (nCommand == 5)
					xmg_cut_rewrite(NewNet, 2);
				else if (nCommand == 6)
					xmg_cut_rewrite(NewNet, 3);
				else if (nCommand == 7)
					xmg_cut_rewrite(NewNet, 4);
				else if (nCommand == 8)
					xmg_resub(NewNet, 5);
				else
				{
					functional_reduction(NewNet);
					NewNet = cleanup_dangling(NewNet);
				}
				/*
				if (NewNet.num_gates() < nNodeMin)
				{
					KeepNet = NewNet.clone();
					nNodeMin = NewNet.num_gates();
				}
				*/
				//if (NewNet.num_gates() > KeepNet.num_gates())
					//NewNet = KeepNet.clone();
				//else
					//KeepNet = NewNet.clone();
				cout << NewNet.num_gates() << "; ";
			}
			int nCh = NewNet.num_gates(); // 保存 Resyn2 迭代前门数。
			while (true)
			{
				Resyn2(NewNet); // 运行一次传统多轮重综合配方。
				if (NewNet.num_gates() >= nCh)
					break;
				nCh = NewNet.num_gates();
			}
			cout << NewNet.num_gates() << "; ";
			cout << "\n";
			if ((NewNet.num_gates() + nOtherNode) <= nAcceptNode)
				break;
			NewNet = Sub.m_net.clone(); // 若超出全网门数预算，放弃本序列并从原子网重启。
		}
		// [06-ADD] e-graph 只作用于刚刚 ExtractSub 得到的独立子网。
		// 但不再用局部门数决定采用哪一方：IMC 原候选和 e-graph 候选
		// 都会经过相同的调度、回填和全局 Pareto (Size, MF) 检验。
		xmg_network EgraphCandidate = NewNet.clone(); // e-graph 从与 IMC 分支相同的传统优化后子网开始。
		// [06-ADD] 默认仍只替换一次以保持已验证实验的可复现性；设置
		// IMC_EGRAPH_REPLACEMENTS=k 才会在同一子网连续尝试 k 次。每一次
		// 都会重新枚举 cut、完整验证等价，最后仍由全局 Size/MF 决定接受。
		int nEgraphReplacements = 1; // 默认仅提交一次局部 e-graph 替换。
		if (const char* configured = std::getenv("IMC_EGRAPH_REPLACEMENTS"))
		{
			int requested = std::atoi(configured); // 将用户环境变量转为正整数替换次数。
			if (requested > 0)
			{
				// [06-ADD][safety] k=3 已确认会在 e-graph 候选进入 IMC
				// 子调度时失败；在定位该接口问题前，最多允许两个“已验证
				// 且可调度”的连续替换，不能静默执行不安全配置。
				constexpr int verified_safe_limit = 2; // 已完成端到端验证的连续替换上限。
				const bool allow_diagnostic_k3 = std::getenv("IMC_EGRAPH_ALLOW_UNSAFE_K") != nullptr; // 明确允许时才解锁诊断 k>=3。
				nEgraphReplacements = allow_diagnostic_k3
					? requested : min(requested, verified_safe_limit);
				if (requested > verified_safe_limit)
				{
					cerr << "[06 e-graph] requested k=" << requested;
					if (allow_diagnostic_k3)
						cerr << " enabled only for diagnostic run\n";
					else
						cerr << " capped at verified-safe k=" << verified_safe_limit << "\n";
				}
			}
		}
		// [06-ADD][diagnostic] 显式关闭 e-graph，供同一输入下的 baseline
		// 路径隔离测试使用；默认不关闭，正常实验行为不变。
		EgraphSubnetStats EgraphStats; // 接收生成阶段的结构、证明与跳过统计。
		const bool disable_egraph = std::getenv("IMC_DISABLE_EGRAPH") != nullptr; // 隔离 A/B 对照时关闭 e-graph。
		// 将最终生效的 k 写入日志：环境变量名拼错时，不能再把默认 k=1
		// 误当作 k=2/k=3 实验。后面的 replacements 则表示实际成功提交的次数。
		cout << "[06 e-graph] configured passes=" << nEgraphReplacements << "\n";
		if (!disable_egraph)
			EgraphStats = TryEgraphOnSubnetwork(EgraphCandidate, nEgraphReplacements);
		else
			cout << "[06 e-graph] disabled by IMC_DISABLE_EGRAPH\n";
		vector<pair<string, xmg_network>> SubCandidates; // 同一轮里要使用相同下游管道评估的子网候选。
		SubCandidates.push_back({ "imc", NewNet.clone() }); // 无论 e-graph 成功与否，原 IMC 候选始终存在。
		if (EgraphStats.replacements > 0)
		{
			cout << "[06 e-graph] subnet gates " << EgraphStats.gates_before
				 << " -> " << EgraphStats.gates_after
				 << ", cut_size=" << EgraphStats.cut_size
				 << ", replacements=" << EgraphStats.replacements
					 << ", candidates=" << EgraphStats.candidates
					 << ", depth=" << EgraphStats.max_depth_before << "->"
					 << EgraphStats.max_depth_after
					 << ", max_fanout=" << EgraphStats.max_fanout_before << "->"
					 << EgraphStats.max_fanout_after
					 << ", selected_mffc_total=" << EgraphStats.selected_mffc_gates_total
					 << ", selected_boundary_total=" << EgraphStats.selected_boundary_inputs_total
					 << ", selected_boundary_max=" << EgraphStats.selected_boundary_inputs_max
					 << ", selected_local_gain_total=" << EgraphStats.selected_local_gain_total
					 << ", zero_gain_replacements=" << EgraphStats.zero_gain_replacements << "\n";
			// [06-ADD][audit] 输出每个 pass 的实际局部替换指纹。它用于解释
			// k 增加后 MF 是否因某一个特定 MFFC 的边界/生命周期而变化。
			for (size_t pass = 0; pass < EgraphStats.selected_root_indices.size(); ++pass)
			{
				cout << "[06 e-graph] pass=" << (pass + 1)
					 << ", root=" << EgraphStats.selected_root_indices[pass]
					 << ", mffc_gates=" << EgraphStats.selected_mffc_gate_counts[pass]
					 << ", boundary_inputs=" << EgraphStats.selected_boundary_input_counts[pass]
					 << ", local_gain=" << EgraphStats.selected_local_gains[pass] << "\n";
			}
			// [06-ADD] 不覆盖 IMC 候选；保留两个分支供全局 Size/MF 比较。
			SubCandidates.push_back({ "egraph", EgraphCandidate.clone() }); // 仅把通过局部完整证明的 e-graph 候选加入 A/B 队列。
		}
		else if (!EgraphStats.batch_complete && EgraphStats.candidates > 0)
		{
			cout << "[06 e-graph] batch incomplete; skip e-graph branch\n";
		}
		else if (EgraphStats.oversized_mffc_skipped > 0 || EgraphStats.oversized_boundary_skipped > 0)
		{
			// [06-ADD][correctness] 超出完整证明范围的候选被明确跳过，不能
			// 把“未验证”误报成“e-graph 无收益”。
			cout << "[06 e-graph] skipped MFFC=" << EgraphStats.oversized_mffc_skipped
					 << ", boundary=" << EgraphStats.oversized_boundary_skipped
					 << " outside exhaustive-proof limits\n";
		}
		else if (!disable_egraph && EgraphStats.candidates > 0)
		{
			// Rust 完成 batch 但没有可提交候选时也必须说明，避免将“无候选”
			// 误解为分支漏执行或调度失败。
			cout << "[06 e-graph] no verified selectable replacement from "
					 << EgraphStats.candidates << " candidates\n";
		}
		// [06-ADD][measurement] 无替换、batch 不完整与成功替换都保留一行生成记录，
		// 这样分析脚本能区分“没有候选”与“候选未被接受”。
		ofMetrics << i << ",generation,egraph," << EgraphStats.candidates << "," // 输出生成阶段一行，无论有无替换都可分析。
			<< EgraphStats.replacements << "," << EgraphStats.gates_before << ","
			<< EgraphStats.gates_after << "," << EgraphStats.max_depth_before << ","
			<< EgraphStats.max_depth_after << "," << EgraphStats.max_fanout_before << ","
			<< EgraphStats.max_fanout_after << "," << EgraphStats.selected_mffc_gates_total << ","
			<< EgraphStats.selected_boundary_inputs_total << ","
			<< EgraphStats.selected_boundary_inputs_max << ","
			<< EgraphStats.selected_local_gain_total << ",,,,,,"
			<< (disable_egraph ? "disabled" : (EgraphStats.replacements > 0 ? "replacement" : "no_replacement")) << "\n";

		// [06-ADD] baseline 分支仍作为后续 ResubMult 的种子，以保持原有
		// DSE 流程；e-graph 分支的首次接受严格由下面的全局 Pareto 决定。
		NetList NewNetList; // 保存 IMC 分支全网候选，作为本轮后续传统 resub 的种子。
		bool bHaveImcSeed = false; // IMC 分支调度成功后才置 true。
		for (auto& SubCandidate : SubCandidates)
		{
			const string& strCandidate = SubCandidate.first; // “imc”或“egraph”，用于日志和差异化超时策略。
			xmg_network& CandidateNet = SubCandidate.second; // 当前待调度的子网实现。
			// [06-ADD][measurement] 多线程调度的 CPU 时间不等于用户实际等待时间；
			// 用墙钟时间覆盖“子调度 + 回填 + ConfigMF”的完整候选评估区间。
			const auto candidate_wall_start = std::chrono::steady_clock::now(); // 从子调度开始测量真实等待时间。
			Scheduler SubScheduler; // 每个候选独立调度，防止状态交叉污染。
			SubScheduler.m_netlist.m_net = CandidateNet.clone(); // 将候选 XMG 交给原 IMC 调度器。
			SubScheduler.m_netlist.ConfigWithXMG(nOrigOffset); // 转换为调度器所需 Node 表并保持原 offset。
			int nNewNodes = CandidateNet.num_gates() + nOtherNode; // 估算回填后的全网门数。
			int nBound = m_nBound; // 先继承顶层 MF 上界。
			for (NetList& nl : vecParetoList)
			{
				if (nl.m_nSize < nNewNodes)
					nBound = min(nBound, nl.m_nMF - 1);
				else if (nl.m_nSize == nNewNodes)
					nBound = min(nBound, nl.m_nMF);
			}
			SubScheduler.m_nBound = nBound * 1.05; // 为整数边界与启发式估计保留 5% 余量。
			if (strCandidate == "egraph")
			{
				// [06-ADD] e-graph 是额外候选；10 秒内不能得到一个可行分区
				// 就跳过，不应阻塞已经可用的 IMC 搜索。可由环境变量放宽。
				SubScheduler.m_nILPTimeLimitSec = 10; // e-graph 额外分支的默认 ILP 限时。
				if (const char* configured = std::getenv("IMC_EGRAPH_ILP_LIMIT_SEC"))
				{
					int requested = std::atoi(configured);
					if (requested > 0) SubScheduler.m_nILPTimeLimitSec = requested;
				}
				SubScheduler.m_nSMTTimeLimitMs = 5000; // e-graph 分支单个 SMT 子问题默认限时 5 秒。
				if (const char* configured = std::getenv("IMC_EGRAPH_SMT_LIMIT_MS"))
				{
					int requested = std::atoi(configured);
					if (requested > 0) SubScheduler.m_nSMTTimeLimitMs = requested;
				}
				SubScheduler.m_nScheduleTimeLimitMs = 30000; // e-graph 整个子网调度默认最多占用 30 秒墙钟时间。
				if (const char* configured = std::getenv("IMC_EGRAPH_SCHEDULE_LIMIT_MS"))
				{
					int requested = std::atoi(configured);
					if (requested > 0) SubScheduler.m_nScheduleTimeLimitMs = requested;
				}
			}
			SubScheduler.ThreadIterPartScheduler(); // 与 IMC 分支相同的“分区 + SMT”管道。
			const auto candidate_schedule_end = std::chrono::steady_clock::now(); // 子调度结束的墙钟时间点。
			const auto schedule_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				candidate_schedule_end - candidate_wall_start).count();
			if (SubScheduler.m_nFootPrint < 1 || SubScheduler.m_bStop)
			{
				cout << "[06 " << strCandidate << "] scheduling failed, wall_ms="
					 << schedule_wall_ms << "\n";
				ofMetrics << i << ",evaluation," << strCandidate << ","
					<< EgraphStats.candidates << "," << EgraphStats.replacements << ",,,,,,,,,,,,,"
					<< schedule_wall_ms << ",," << "scheduling_failed\n";
				continue;
			}

			NetList NewSub = SubScheduler.m_netlist.ReorderNodes(); // 将子网调度顺序固化为新的局部 Node 顺序。
			NetList CandidateNetList = OrigNet.SubstituteSub(nBegin, nEnd, NewSub, vecTIOrigIndex, vecPOOrigIndex); // 用共同边界映射回填完整网络。
			CandidateNetList.ConfigMF(); // 最终判断依据：回填全网后的真实 Size、MF、Cross。
			const auto candidate_wall_end = std::chrono::steady_clock::now();
			const auto total_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				candidate_wall_end - candidate_wall_start).count();
			cout << "[06 " << strCandidate << "] global: #nodes = "
				 << CandidateNetList.m_nSize << ", MF = " << CandidateNetList.m_nMF
				 << ", schedule_wall_ms=" << schedule_wall_ms
				 << ", total_wall_ms=" << total_wall_ms << "\n";
			ofCSV << CandidateNetList.m_nSize << ", " << CandidateNetList.m_nMF << ","
				 << CandidateNetList.m_nCross << "," << strCandidate << "\n";
			ofMetrics << i << ",evaluation," << strCandidate << ","
				<< EgraphStats.candidates << "," << EgraphStats.replacements << ",,,,,,,,,,,"
				<< CandidateNetList.m_nSize << "," << CandidateNetList.m_nMF << ","
				<< CandidateNetList.m_nCross << "," << schedule_wall_ms << ","
				<< total_wall_ms << ",completed\n";
			bool bIntoPareto = UpdatePareto(CandidateNetList, vecParetoList); // 不以局部门数而以全局二目标决定是否保留。
			cout << "[06 " << strCandidate << "] "
				 << (bIntoPareto ? "Into Pareto" : "Not Pareto") << "\n";

			if (strCandidate == "imc")
			{
				NewNetList = CandidateNetList; // 记住 IMC 候选供兼容的后续传统 resub 使用。
				bHaveImcSeed = true; // 表示本轮可安全进入后续原始流程。
			}
		}
		if (!bHaveImcSeed)
			continue; // IMC 基准分支本身失败时，跳过本轮，避免改变基线语义。

		NetList EmptyList;
		clock_t begin = clock();
		cout << "Begin resub\n";
		int nNumSub = 0;
		NetList netlist = EmptyList;
		netlist.m_net = NewNetList.m_net.clone();
		netlist.ConfigWithXMG();
		netlist.m_vecnSchedule.clear();
		for (int i = 0; i < netlist.m_vecNode.size(); i++)
			netlist.m_vecnSchedule.push_back(i);
		netlist.ConfigMF();
		xmg_network orig_net = netlist.m_net.clone();
		while (true)
		{
			if (!ResubMult(netlist))
				break;
			nNumSubb++;
			nNumSub++;
			cout << nNumSub << "-th resub\n";
			xmg_network new_net = netlist.ConstructXMG();
			netlist = EmptyList;
			netlist.m_net = new_net.clone();
			netlist.ConfigWithXMG();
			netlist.m_vecnSchedule.clear();
			for (int i = 0; i < netlist.m_vecNode.size(); i++)
				netlist.m_vecnSchedule.push_back(i);
			netlist.ConfigMF();
			cout << "New netlist: #nodes = " << netlist.m_nSize << ", MF = " << netlist.m_nMF << "\n";
			ofCSV << netlist.m_nSize << ", " << netlist.m_nMF << "," << netlist.m_nCross << "," << 1 << "\n";
			if (UpdatePareto(netlist, vecParetoList))
				cout << "Into Pareto\n";
			else
				cout << "Not Pareto\n";

			if (netlist.DelRedun())
			{
				cout << "delete reduntant---------------------\n";
				xmg_network newnew_net = netlist.ConstructXMG();
				netlist = EmptyList;
				netlist.m_net = newnew_net.clone();
				netlist.ConfigWithXMG();
				netlist.m_vecnSchedule.clear();
				for (int i = 0; i < netlist.m_vecNode.size(); i++)
					netlist.m_vecnSchedule.push_back(i);
				netlist.ConfigMF();
				cout << "New netlist: #nodes = " << netlist.m_nSize << ", MF = " << netlist.m_nMF << "\n";
				ofCSV << netlist.m_nSize << ", " << netlist.m_nMF << "," << netlist.m_nCross << "," << 1 << "\n";
				if (UpdatePareto(netlist, vecParetoList))
					cout << "Into Pareto\n";
				else
					cout << "Not Pareto\n";
			}
			else
				cout << "no reduntant nodes--------------------\n";
		}
		cout << "End resub\n";
		cout << "Time = " << double(clock() - begin) / CLOCKS_PER_SEC << ", #resub = " << nNumSub << "\n";
		ofCSV.flush();
	}
	ofCSV.close();
	ofMetrics.close();
}
