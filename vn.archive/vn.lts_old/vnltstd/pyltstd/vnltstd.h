//说明部分

//系统
#include "stdafx.h"
#include <string>
#include <queue>

//Boost
#define BOOST_PYTHON_STATIC_LIB
#include <boost/python/module.hpp>	//python封装
#include <boost/python/def.hpp>		//python封装
#include <boost/python/dict.hpp>	//python封装
#include <boost/python/object.hpp>	//python封装
#include <boost/python.hpp>			//python封装
#include <boost/thread.hpp>			//任务队列的线程功能
#include <boost/bind.hpp>			//任务队列的线程功能
#include <boost/any.hpp>			//任务队列的任务实现

//API
#include "SecurityFtdcTraderApi.h"

//命名空间
using namespace std;
using namespace boost::python;
using namespace boost;

//常量
#define ONFRONTCONNECTED 1
#define ONFRONTDISCONNECTED 2
#define ONHEARTBEATWARNING 3
#define ONRSPERROR 4
#define ONRSPUSERLOGIN 5
#define ONRSPUSERLOGOUT 6
#define ONRSPORDERINSERT 7
#define ONRSPORDERACTION 8
#define ONRSPUSERPASSWORDUPDATE 9
#define ONRSPTRADINGACCOUNTPASSWORDUPDATE 10
#define ONRSPQRYEXCHANGE 11
#define ONRSPQRYINSTRUMENT 12
#define ONRSPQRYINVESTOR 13
#define ONRSPQRYTRADINGCODE 14
#define ONRSPQRYTRADINGACCOUNT 15
#define ONRSPQRYDEPTHMARKETDATA 16
#define ONRSPQRYBONDINTEREST 17
#define ONRSPQRYMARKETRATIONINFO 18
#define ONRSPQRYINSTRUMENTCOMMISSIONRATE 19
#define ONRSPQRYETFINSTRUMENT 20
#define ONRSPQRYETFBASKET 21
#define ONRSPQRYOFINSTRUMENT 22
#define ONRSPQRYSFINSTRUMENT 23
#define ONRSPQRYORDER 24
#define ONRSPQRYTRADE 25
#define ONRSPQRYINVESTORPOSITION 26
#define ONRTNORDER 27
#define ONRTNTRADE 28
#define ONERRRTNORDERINSERT 29
#define ONERRRTNORDERACTION 30
#define ONRSPFUNDOUTBYLIBER 31
#define ONRTNFUNDOUTBYLIBER 32
#define ONERRRTNFUNDOUTBYLIBER 33
#define ONRTNFUNDINBYBANK 34
#define ONRSPQRYFUNDTRANSFERSERIAL 35
#define ONRSPFUNDINTERTRANSFER 36
#define ONRSPQRYFUNDINTERTRANSFERSERIAL 37
#define ONRTNFUNDINTERTRANSFERSERIAL 38
#define ONERRRTNFUNDINTERTRANSFER 39


///-------------------------------------------------------------------------------------
///API中的部分组件
///-------------------------------------------------------------------------------------

//GIL全局锁简化获取用，
//用于帮助C++线程获得GIL锁，从而防止python崩溃
class PyLock
{
private:
	PyGILState_STATE gil_state;

public:
	//在某个函数方法中创建该对象时，获得GIL锁
	PyLock()
	{
		gil_state = PyGILState_Ensure();
	}

	//在某个函数完成后销毁该对象时，解放GIL锁
	~PyLock()
	{
		PyGILState_Release(gil_state);
	}
};


//任务结构体
struct Task
{
	int task_name;		//回调函数名称对应的常量
	any task_data;		//数据结构体
	any task_error;		//错误结构体
	int task_id;		//请求id
	bool task_last;		//是否为最后返回
};


///线程安全的队列
template<typename Data>

class ConcurrentQueue
{
private:
	queue<Data> the_queue;								//标准库队列
	mutable mutex the_mutex;							//boost互斥锁
	condition_variable the_condition_variable;			//boost条件变量

public:

	//存入新的任务
	void push(Data const& data)
	{
		mutex::scoped_lock lock(the_mutex);				//获取互斥锁
		the_queue.push(data);							//向队列中存入数据
		lock.unlock();									//释放锁
		the_condition_variable.notify_one();			//通知正在阻塞等待的线程
	}

	//检查队列是否为空
	bool empty() const
	{
		mutex::scoped_lock lock(the_mutex);
		return the_queue.empty();
	}

	//取出
	Data wait_and_pop()
	{
		mutex::scoped_lock lock(the_mutex);

		while (the_queue.empty())						//当队列为空时
		{
			the_condition_variable.wait(lock);			//等待条件变量通知
		}

		Data popped_value = the_queue.front();			//获取队列中的最后一个任务
		the_queue.pop();								//删除该任务
		return popped_value;							//返回该任务
	}

};


//从字典中获取某个建值对应的整数，并赋值到请求结构体对象的值上
void getInt(dict d, string key, int* value);


//从字典中获取某个建值对应的浮点数，并赋值到请求结构体对象的值上
void getDouble(dict d, string key, double* value);


//从字典中获取某个建值对应的字符串，并赋值到请求结构体对象的值上
void getChar(dict d, string key, char* value);



///-------------------------------------------------------------------------------------
///C++ SPI的回调函数方法实现
///-------------------------------------------------------------------------------------

//API的继承实现
class TdApi : public CSecurityFtdcTraderSpi
{
private:
	CSecurityFtdcTraderApi* api;			//API对象
	thread *task_thread;				//工作线程指针（向python中推送数据）
	ConcurrentQueue<Task> task_queue;	//任务队列

public:
	TdApi()
	{
		function0<void> f = boost::bind(&TdApi::processTask, this);
		thread t(f);
		this->task_thread = &t;
	};

	~TdApi()
	{
	};

	//-------------------------------------------------------------------------------------
	//API回调函数
	//-------------------------------------------------------------------------------------

	///当客户端与交易后台建立起通信连接时（还未登录前），该方法被调用。
	virtual void OnFrontConnected();

	///当客户端与交易后台通信连接断开时，该方法被调用。当发生这个情况后，API会自动重新连接，客户端可不做处理。
	///@param nReason 错误原因
	///        0x1001 网络读失败
	///        0x1002 网络写失败
	///        0x2001 接收心跳超时
	///        0x2002 发送心跳失败
	///        0x2003 收到错误报文
	virtual void OnFrontDisconnected(int nReason);

	///心跳超时警告。当长时间未收到报文时，该方法被调用。
	///@param nTimeLapse 距离上次接收报文的时间
	virtual void OnHeartBeatWarning(int nTimeLapse);

	///错误应答
	virtual void OnRspError(CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///登录请求响应
	virtual void OnRspUserLogin(CSecurityFtdcRspUserLoginField *pRspUserLogin, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///登出请求响应
	virtual void OnRspUserLogout(CSecurityFtdcUserLogoutField *pUserLogout, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///报单录入请求响应
	virtual void OnRspOrderInsert(CSecurityFtdcInputOrderField *pInputOrder, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///报单操作请求响应
	virtual void OnRspOrderAction(CSecurityFtdcInputOrderActionField *pInputOrderAction, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///用户口令更新请求响应
	virtual void OnRspUserPasswordUpdate(CSecurityFtdcUserPasswordUpdateField *pUserPasswordUpdate, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///资金账户口令更新请求响应
	virtual void OnRspTradingAccountPasswordUpdate(CSecurityFtdcTradingAccountPasswordUpdateField *pTradingAccountPasswordUpdate, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询交易所响应
	virtual void OnRspQryExchange(CSecurityFtdcExchangeField *pExchange, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询合约响应
	virtual void OnRspQryInstrument(CSecurityFtdcInstrumentField *pInstrument, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询投资者响应
	virtual void OnRspQryInvestor(CSecurityFtdcInvestorField *pInvestor, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询交易编码响应
	virtual void OnRspQryTradingCode(CSecurityFtdcTradingCodeField *pTradingCode, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询资金账户响应
	virtual void OnRspQryTradingAccount(CSecurityFtdcTradingAccountField *pTradingAccount, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询行情响应
	virtual void OnRspQryDepthMarketData(CSecurityFtdcDepthMarketDataField *pDepthMarketData, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询债券利息响应
	virtual void OnRspQryBondInterest(CSecurityFtdcBondInterestField *pBondInterest, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询市值配售信息响应
	virtual void OnRspQryMarketRationInfo(CSecurityFtdcMarketRationInfoField *pMarketRationInfo, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询合约手续费率响应
	virtual void OnRspQryInstrumentCommissionRate(CSecurityFtdcInstrumentCommissionRateField *pInstrumentCommissionRate, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询ETF合约响应
	virtual void OnRspQryETFInstrument(CSecurityFtdcETFInstrumentField *pETFInstrument, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询ETF股票篮响应
	virtual void OnRspQryETFBasket(CSecurityFtdcETFBasketField *pETFBasket, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询OF合约响应
	virtual void OnRspQryOFInstrument(CSecurityFtdcOFInstrumentField *pOFInstrument, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询SF合约响应
	virtual void OnRspQrySFInstrument(CSecurityFtdcSFInstrumentField *pSFInstrument, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询报单响应
	virtual void OnRspQryOrder(CSecurityFtdcOrderField *pOrder, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询成交响应
	virtual void OnRspQryTrade(CSecurityFtdcTradeField *pTrade, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///请求查询投资者持仓响应
	virtual void OnRspQryInvestorPosition(CSecurityFtdcInvestorPositionField *pInvestorPosition, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///报单通知
	virtual void OnRtnOrder(CSecurityFtdcOrderField *pOrder);

	///成交通知
	virtual void OnRtnTrade(CSecurityFtdcTradeField *pTrade);

	///报单录入错误回报
	virtual void OnErrRtnOrderInsert(CSecurityFtdcInputOrderField *pInputOrder, CSecurityFtdcRspInfoField *pRspInfo);

	///报单操作错误回报
	virtual void OnErrRtnOrderAction(CSecurityFtdcOrderActionField *pOrderAction, CSecurityFtdcRspInfoField *pRspInfo);

	///Liber发起出金应答
	virtual void OnRspFundOutByLiber(CSecurityFtdcInputFundTransferField *pInputFundTransfer, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///Liber发起出金通知
	virtual void OnRtnFundOutByLiber(CSecurityFtdcFundTransferField *pFundTransfer);

	///Liber发起出金错误回报
	virtual void OnErrRtnFundOutByLiber(CSecurityFtdcInputFundTransferField *pInputFundTransfer, CSecurityFtdcRspInfoField *pRspInfo);

	///银行发起入金通知
	virtual void OnRtnFundInByBank(CSecurityFtdcFundTransferField *pFundTransfer);

	///资金转账查询应答
	virtual void OnRspQryFundTransferSerial(CSecurityFtdcFundTransferField *pFundTransfer, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///资金内转应答
	virtual void OnRspFundInterTransfer(CSecurityFtdcFundInterTransferField *pFundInterTransfer, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///资金内转流水查询应答
	virtual void OnRspQryFundInterTransferSerial(CSecurityFtdcFundInterTransferSerialField *pFundInterTransferSerial, CSecurityFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///资金内转流水通知
	virtual void OnRtnFundInterTransferSerial(CSecurityFtdcFundInterTransferSerialField *pFundInterTransferSerial);

	///资金内转错误回报
	virtual void OnErrRtnFundInterTransfer(CSecurityFtdcFundInterTransferField *pFundInterTransfer, CSecurityFtdcRspInfoField *pRspInfo);

	//-------------------------------------------------------------------------------------
	//task：任务
	//-------------------------------------------------------------------------------------

	void processTask();

	void processFrontConnected(Task task);

	void processFrontDisconnected(Task task);

	void processHeartBeatWarning(Task task);

	void processRspError(Task task);

	void processRspUserLogin(Task task);

	void processRspUserLogout(Task task);

	void processRspOrderInsert(Task task);

	void processRspOrderAction(Task task);

	void processRspUserPasswordUpdate(Task task);

	void processRspTradingAccountPasswordUpdate(Task task);

	void processRspQryExchange(Task task);

	void processRspQryInstrument(Task task);

	void processRspQryInvestor(Task task);

	void processRspQryTradingCode(Task task);

	void processRspQryTradingAccount(Task task);

	void processRspQryDepthMarketData(Task task);

	void processRspQryBondInterest(Task task);

	void processRspQryMarketRationInfo(Task task);

	void processRspQryInstrumentCommissionRate(Task task);

	void processRspQryETFInstrument(Task task);

	void processRspQryETFBasket(Task task);

	void processRspQryOFInstrument(Task task);

	void processRspQrySFInstrument(Task task);

	void processRspQryOrder(Task task);

	void processRspQryTrade(Task task);

	void processRspQryInvestorPosition(Task task);

	void processRtnOrder(Task task);

	void processRtnTrade(Task task);

	void processErrRtnOrderInsert(Task task);

	void processErrRtnOrderAction(Task task);

	void processRspFundOutByLiber(Task task);

	void processRtnFundOutByLiber(Task task);

	void processErrRtnFundOutByLiber(Task task);

	void processRtnFundInByBank(Task task);

	void processRspQryFundTransferSerial(Task task);

	void processRspFundInterTransfer(Task task);

	void processRspQryFundInterTransferSerial(Task task);

	void processRtnFundInterTransferSerial(Task task);

	void processErrRtnFundInterTransfer(Task task);


	//-------------------------------------------------------------------------------------
	//data：回调函数的数据字典
	//error：回调函数的错误字典
	//id：请求id
	//last：是否为最后返回
	//i：整数
	//-------------------------------------------------------------------------------------

	virtual void onFrontConnected(){};

	virtual void onFrontDisconnected(int i){};

	virtual void onHeartBeatWarning(int i){};

	virtual void onRspError(dict error, int id, bool last) {};

	virtual void onRspUserLogin(dict data, dict error, int id, bool last) {};

	virtual void onRspUserLogout(dict data, dict error, int id, bool last) {};

	virtual void onRspOrderInsert(dict data, dict error, int id, bool last) {};

	virtual void onRspOrderAction(dict data, dict error, int id, bool last) {};

	virtual void onRspUserPasswordUpdate(dict data, dict error, int id, bool last) {};

	virtual void onRspTradingAccountPasswordUpdate(dict data, dict error, int id, bool last) {};

	virtual void onRspQryExchange(dict data, dict error, int id, bool last) {};

	virtual void onRspQryInstrument(dict data, dict error, int id, bool last) {};

	virtual void onRspQryInvestor(dict data, dict error, int id, bool last) {};

	virtual void onRspQryTradingCode(dict data, dict error, int id, bool last) {};

	virtual void onRspQryTradingAccount(dict data, dict error, int id, bool last) {};

	virtual void onRspQryDepthMarketData(dict data, dict error, int id, bool last) {};

	virtual void onRspQryBondInterest(dict data, dict error, int id, bool last) {};

	virtual void onRspQryMarketRationInfo(dict data, dict error, int id, bool last) {};

	virtual void onRspQryInstrumentCommissionRate(dict data, dict error, int id, bool last) {};

	virtual void onRspQryETFInstrument(dict data, dict error, int id, bool last) {};

	virtual void onRspQryETFBasket(dict data, dict error, int id, bool last) {};

	virtual void onRspQryOFInstrument(dict data, dict error, int id, bool last) {};

	virtual void onRspQrySFInstrument(dict data, dict error, int id, bool last) {};

	virtual void onRspQryOrder(dict data, dict error, int id, bool last) {};

	virtual void onRspQryTrade(dict data, dict error, int id, bool last) {};

	virtual void onRspQryInvestorPosition(dict data, dict error, int id, bool last) {};

	virtual void onRtnOrder(dict data) {};

	virtual void onRtnTrade(dict data) {};

	virtual void onErrRtnOrderInsert(dict data, dict error) {};

	virtual void onErrRtnOrderAction(dict data, dict error) {};

	virtual void onRspFundOutByLiber(dict data, dict error, int id, bool last) {};

	virtual void onRtnFundOutByLiber(dict data) {};

	virtual void onErrRtnFundOutByLiber(dict data, dict error) {};

	virtual void onRtnFundInByBank(dict data) {};

	virtual void onRspQryFundTransferSerial(dict data, dict error, int id, bool last) {};

	virtual void onRspFundInterTransfer(dict data, dict error, int id, bool last) {};

	virtual void onRspQryFundInterTransferSerial(dict data, dict error, int id, bool last) {};

	virtual void onRtnFundInterTransferSerial(dict data) {};

	virtual void onErrRtnFundInterTransfer(dict data, dict error) {};

	//-------------------------------------------------------------------------------------
	//req:主动函数的请求字典
	//-------------------------------------------------------------------------------------

	void createFtdcTraderApi(string pszFlowPath = "");

	void release();

	void init();

	int join();

	int exit();

	string getTradingDay();

	void registerFront(string pszFrontAddress);

	void subscribePrivateTopic(int nType);

	void subscribePublicTopic(int nType);

	int reqUserLogin(dict req, int nRequestID);

	int reqUserLogout(dict req, int nRequestID);

	int reqOrderInsert(dict req, int nRequestID);

	int reqOrderAction(dict req, int nRequestID);

	int reqUserPasswordUpdate(dict req, int nRequestID);

	int reqTradingAccountPasswordUpdate(dict req, int nRequestID);

	int reqQryExchange(dict req, int nRequestID);

	int reqQryInstrument(dict req, int nRequestID);

	int reqQryInvestor(dict req, int nRequestID);

	int reqQryTradingCode(dict req, int nRequestID);

	int reqQryTradingAccount(dict req, int nRequestID);

	int reqQryDepthMarketData(dict req, int nRequestID);

	int reqQryBondInterest(dict req, int nRequestID);

	int reqQryMarketRationInfo(dict req, int nRequestID);

	int reqQryInstrumentCommissionRate(dict req, int nRequestID);

	int reqQryETFInstrument(dict req, int nRequestID);

	int reqQryETFBasket(dict req, int nRequestID);

	int reqQryOFInstrument(dict req, int nRequestID);

	int reqQrySFInstrument(dict req, int nRequestID);

	int reqQryOrder(dict req, int nRequestID);

	int reqQryTrade(dict req, int nRequestID);

	int reqQryInvestorPosition(dict req, int nRequestID);

	int reqFundOutByLiber(dict req, int nRequestID);

	int reqQryFundTransferSerial(dict req, int nRequestID);

	int reqFundInterTransfer(dict req, int nRequestID);

	int reqQryFundInterTransferSerial(dict req, int nRequestID);
};
