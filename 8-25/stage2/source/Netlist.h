#pragma once // 防止头文件重复展开。
#include "utils.h" // 提供 STL、XMG、求解器等 NetList 所需类型。

// Node 是调度器使用的逻辑门记录；与 mockturtle 内部 node 类型不同。
class Node
{
public:
	// 初始化边界/常量标记，避免未配置节点被误认为 PO 或常量门。
	Node()
	{
		m_bPO = false; // 默认不驱动 primary output。
		m_nOrigIndex = 0; // 原始编号尚未映射时的占位值。
		m_nConstPI = -1; // -1 代表没有常量前驱。
	}
	vector<int> m_vecnSucc; // 后继逻辑门编号。
	vector<int> m_vecnPred; // 逻辑门前驱编号。
	vector<bool> m_vecbPredComp; // 对应逻辑门输入边的反相位。
	vector<int> m_vecnPredPI; // PI 或子网 TI 前驱编号。
	vector<bool> m_vecbPredPIComp; // 对应 PI/TI 边的反相位。
	unsigned int m_nIndex; // 当前 NetList 中的节点编号。
	unsigned int m_nOrigIndex; // 子网回填使用的完整网络编号。
	bool m_bPO; // 是否为主输出驱动门。
	int m_nPartition; // ILP 分区标签。
	bool m_bMAJ; // true 表示 MAJ，false 表示 XOR3。
	int m_nConstPI; // -1=无常量，0/1=常量输入。
	std::set<int> m_setnConePI; // [06-MOD] 避免 nauty 导出的全局 set 名冲突。
};


// NetList 是 XMG 逻辑图与 IMC 调度数据之间的桥梁。
class NetList
{
public:
	// 默认硬件：4 个阵列、每阵列 253 行。
	NetList() 
	{
		m_nArrayRow = 253; // 单阵列行容量。
		m_nNumArray = 4; // 阵列数量。
	}
	NetList(vector<Node> vecNode, vector<Node> vecIn, vector<int> vecnPo); // 使用已有节点记录构造网表。
	void ReadFromFile(string strFile); // 读取输入电路并构建 XMG。
	void ConfigWithXMG(int nOrigOffset = 1000000); // XMG → 调度节点表。
	void ConfigMF(); // 固定调度顺序下计算 Size/MF/Cross。

	xmg_network ConstructXMG(int nOrigOffset = 1000000); // 调度节点表 → XMG，供重写器操作。
	NetList ReorderNodes(); // 根据调度顺序产生重排副本。
	vector<int> GetPeak(); // 找峰值资源区间，作为子网优化候选区域。
	void SubPred(int nNode, vector<int>& vecnPredID, vector<bool>& vecbPredComp, bool bMaj, bool bComp); // 处理替换门的前驱。
	void SubNode(int nSmall, int nLarge, bool bComp); // 将小门的用途重定向到大门。
	bool DelRedun(); // 清除替换后无效的冗余节点。
	NetList ExtractSub(int nBegin, int nEnd, vector<int>& vecTIOrigIndex, vector<int>& vecPOOrigIndex); // 从完整网表切出子网。
	NetList SubstituteSub(int nBegin, int nEnd, const NetList& NewSub, vector<int>& vecTIOrigIndex, vector<int>& vecPOOrigIndex); // 以新子网回填完整网表。

	xmg_network m_net; // 用于逻辑优化的 XMG。
	int m_nOffset; // XMG 编号和调度编号之间的偏移。
	vector<Node> m_vecNode; // 可调度逻辑门列表。
	vector<Node> m_vecIn; // 输入辅助列表。
	vector<int> m_vecnPO; // PO 驱动门列表。
	vector<int> m_vecnSchedule; // 当前门执行顺序。
	vector<int> m_vecnPOIndex; // PO 序号映射。
	vector<bool> m_vecbPOComp; // PO 极性映射。
	int m_nMF; // 映射资源行数。
	int m_nSize; // 逻辑门数量。
	int m_nCross; // 跨阵列通信统计。
	int m_nNumArray; // 阵列总数。
	int m_nArrayRow; // 每阵列容量。
	int m_nNumPI; // PI 数量。
	string m_strBench; // benchmark 名称。

	vector<int> m_vecMF; // 每步资源占用轨迹。
	vector<int> m_vecMaxMFIndex; // 峰值位置轨迹。
};
