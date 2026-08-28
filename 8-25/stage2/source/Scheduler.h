#pragma once // 防止 Scheduler 被重复定义。
#include "NetList.h" // Scheduler 以 NetList/Node 为核心数据类型。

// Scheduler 驱动 baseline 调度和 06 的严格 IMC/e-graph A/B 候选比较。
class Scheduler
{
public:
	Scheduler(); // 设置默认资源、线程和求解时限。
	int CallSMT(NetList& netlist, int nMF); // 测试给定 MF 下是否存在合法子网调度。
	int BiSMT(NetList& netlist, int nUpper, int nLower); // 二分搜索低 MF 调度。
	int PartitionILP(NetList& netlist, double dPOWeight = 1); // Gurobi 分区；06 增加失败安全返回。
	void IterPart(NetList& netlist, double dLeft = 1, double dRight = 1); // 递归执行分区/调度。
	void ScheduleThread(); // 子网调度工作线程函数。
	void ThreadIterPartScheduler(); // 调度所有分区并汇总执行顺序。

	bool IsDivisor(const Node& ndRoot, const Node& ndDiv); // 判断候选除数是否不会形成非法循环。
	void ResubThread(); // 并行重代入线程入口。
	bool ResubMult(NetList& netlist); // 尝试一次 baseline 逻辑重代入。
	bool UpdatePareto(NetList& NewNetList, vector<NetList>& vecParetoList); // 统一接受 IMC/e-graph 全网候选。
	void NewDSE(); // 06 的主循环：其中包含 e-graph 分支和指标 CSV。

	NetList m_netlist; // 当前完整网络。
	int m_nFootPrint; // 当前调度的资源足迹。
	vector<NetList> m_vecPartNet; // 分区后的子网列表。
	vector<int> m_vecMF; // 调度过程记录的 MF 值。
	int m_nMFLow; // 当前低 MF 界。
	bool m_bStop; // 超时/失败时停止递归调度。

	bool m_bFoundResub; // 是否发现可替换的逻辑候选。
	volatile LONG m_nCheckPeak; // 并行峰值检查游标。
	vector<kitty::partial_truth_table> m_tts; // 局部函数真值表缓存。
	vector<bool> m_vecPhase; // 极性选择缓存。
	vector<int> m_vecPeakIndex; // 峰值节点索引。
	volatile LONG m_nVFail; // 并行验证失败数。
	volatile LONG m_nRoot; // 待处理根节点游标。
	NetList m_curNetlist; // 原网络工作副本。
	NetList m_newNetlist; // 修改后网络工作副本。
	
	CRITICAL_SECTION m_rCritical; // 保护共享计数器与候选。
	HANDLE	m_hEventKillThread; // 通知线程停止的事件对象。
	int m_nTotNet; // 本轮子网总数。
	volatile LONG m_nProcNet; // 已处理子网数。

	int m_nBound; // 当前实验的 MF 上界。
	double m_epsilon; // 浮点支配判断容差。
	int m_nThread; // 调度线程数，可由 IMC_THREADS 配置。
	// 设置 IMC_DETERMINISTIC=1 时启用：所有会改变候选接受顺序的并发路径均
	// 串行执行，Gurobi/Z3 也各自限制为一个内部线程。它用于可复现实验，
	// 不应用于追求吞吐的默认 DSE。
	bool m_bDeterministic;
	int m_nGraphBound; // 分区子网规模限制。
	int m_nILPTimeLimitSec; // ILP 求解时限（秒）。
	int m_nSMTTimeLimitMs; // SMT 求解时限（毫秒）。
	DWORD m_nScheduleTimeLimitMs; // 整次子网调度的等待上限（毫秒）。
	int m_nProgressDone; // 已完成搜索工作量。
	int m_nProgressTotal; // 总搜索工作量。
	double m_dPO; // 分区时 PO 边界权重。

	double m_increase; // DSE 搜索的增量/扰动参数。
	double m_critical; // MF 关键区间阈值。
	int m_nRun; // DSE 迭代轮数。
};
