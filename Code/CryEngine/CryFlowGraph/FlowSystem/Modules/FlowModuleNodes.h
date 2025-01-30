// Copyright 2012-2021 Crytek GmbH / Crytek Group. All rights reserved.

/*
 * Utility nodes and Node types for modules: Start, Return and Call.
 * These nodes are generated by factories with the following convention:
 *  Module:[Call|Start|Return]_<ModuleName>
 */

#pragma once

#include <CryFlowGraph/IFlowGraphModuleManager.h>
#include <CryFlowGraph/IFlowBaseNode.h>

class CFlowGraphModule;
struct SModuleInstance;

//////////////////////////////////////////////////////////////////////////
//
//	Dynamically generated nodes based on a module's inputs/outputs
//
//////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
// Start node factory
//////////////////////////////////////////////////////////////////////////

class CFlowModuleStartNodeFactory : public IFlowNodeFactory
{
public:
	CFlowModuleStartNodeFactory(CFlowGraphModule* pModule);
	~CFlowModuleStartNodeFactory();

	virtual IFlowNodePtr Create(IFlowNode::SActivationInfo*) override;

	virtual void         GetMemoryUsage(ICrySizer* s) const override
	{
		SIZER_SUBCOMPONENT_NAME(s, "CFlowModuleStartNodeFactory");
		s->AddObject(this, sizeof(*this));
		s->AddObject(m_outputs);
	}

	void         GetConfiguration(SFlowNodeConfig&);

	void         Reset() override;

	virtual bool AllowOverride() const override { return true; }

private:
	friend class CFlowModuleStartNode;
	CFlowGraphModule* const        m_pModule;

	TModuleId                      m_moduleId;
	std::vector<SOutputPortConfig> m_outputs;
};

//////////////////////////////////////////////////////////////////////////
// Start node inside the module.
// Dynamically generated by a factory with ports from the Module's interface
//////////////////////////////////////////////////////////////////////////

class CFlowModuleStartNode : public CFlowBaseNode<eNCT_Instanced>
{
public:
	CFlowModuleStartNode(CFlowModuleStartNodeFactory* pFactory, SActivationInfo* pActInfo);
	virtual ~CFlowModuleStartNode() {}

	virtual IFlowNodePtr Clone(SActivationInfo* pActInfo);
	virtual void         GetMemoryUsage(ICrySizer* s) const { s->Add(*this); }

	virtual void         GetConfiguration(SFlowNodeConfig&);
	virtual void         ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo);

	void                 OnUpdateAllInputs(TModuleParams const* params);
	void                 OnUpdateSingleInput(size_t idx, const TFlowInputData& value);
	void OnCancel();

private:
	void Serialize(SActivationInfo* pActInfo, TSerialize ser) {}

	enum EOutputPorts
	{
		eOP_EntityId,
		eOP_Start,
		eOP_Update,
		eOP_Cancel,
		eOP_Param1,
		// ...
	};
	SActivationInfo                    m_actInfo;

	CFlowModuleStartNodeFactory* const m_pFactory;   //! Reference to the factory that generated this node (has ports info)
	CFlowGraphModule* const            m_pModule;    //! Reference to the Module type for this node
	EntityId                           m_entityId;   //! Entity that this instance is associated with, may be invalid
	TModuleInstanceId                  m_instanceId; //! ID of the module instance that this node is associated it
	                                                 // Changes at runtime and might be invalid (start nodes are cloned)

	bool m_bStarted;
};

//////////////////////////////////////////////////////////////////////////
// Return node factory
//////////////////////////////////////////////////////////////////////////

class CFlowModuleReturnNodeFactory : public IFlowNodeFactory
{
public:
	CFlowModuleReturnNodeFactory(CFlowGraphModule* pModule);
	~CFlowModuleReturnNodeFactory();

	virtual IFlowNodePtr Create(IFlowNode::SActivationInfo*) override;

	virtual void         GetMemoryUsage(ICrySizer* s) const override
	{
		SIZER_SUBCOMPONENT_NAME(s, "CFlowModuleReturnNodeFactory");
		s->AddObject(this, sizeof(*this));
		s->AddObject(m_inputs);
	}

	void         GetConfiguration(SFlowNodeConfig&);

	void         Reset() override;

	virtual bool AllowOverride() const override { return true; }

private:
	friend class CFlowModuleReturnNode;
	CFlowGraphModule* const       m_pModule;

	TModuleId                     m_moduleId;
	std::vector<SInputPortConfig> m_inputs;
};

//////////////////////////////////////////////////////////////////////////
// Return (end) node inside the module.
// Dynamically generated by a factory with ports from the Module's interface
//////////////////////////////////////////////////////////////////////////

class CFlowModuleReturnNode : public CFlowBaseNode<eNCT_Instanced>
{
public:
	CFlowModuleReturnNode(CFlowModuleReturnNodeFactory* pFactory, SActivationInfo* pActInfo);
	virtual ~CFlowModuleReturnNode() {}

	virtual IFlowNodePtr Clone(SActivationInfo* pActInfo);
	virtual void         GetMemoryUsage(ICrySizer* s) const { s->Add(*this); }

	virtual void         GetConfiguration(SFlowNodeConfig&);
	virtual void         ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo);

	void                 OnFinished(bool success);

private:
	void Serialize(SActivationInfo* pActInfo, TSerialize ser) {}

	enum EInputPorts
	{
		eIP_Success = 0,
		eIP_Cancel,
		eIP_Param1,
		// ...
	};
	SActivationInfo                     m_actInfo;

	CFlowModuleReturnNodeFactory* const m_pFactory;   //! Reference to the factory that generated this node (has ports info)
	CFlowGraphModule* const             m_pModule;    //! Reference to the Module type for this node
	EntityId                            m_entityId;   //! Entity that this instance is associated with, may be invalid
	TModuleInstanceId                   m_instanceId; //! ID of the module instance that this node is associated it
	                                                  // Changes at runtime and might be invalid (return nodes are cloned)
	bool                                m_bFinished;
};

//////////////////////////////////////////////////////////////////////////
// Call module node factory
//////////////////////////////////////////////////////////////////////////

class CFlowModuleCallNodeFactory : public IFlowNodeFactory
{
public:
	CFlowModuleCallNodeFactory(CFlowGraphModule* pModule);
	~CFlowModuleCallNodeFactory();

	virtual IFlowNodePtr Create(IFlowNode::SActivationInfo*) override;

	virtual void         GetMemoryUsage(ICrySizer* s) const override
	{
		SIZER_SUBCOMPONENT_NAME(s, "CFlowModuleCallNodeFactory");
		s->AddObject(this, sizeof(*this));
		s->AddObject(m_outputs);
	}

	void         GetConfiguration(SFlowNodeConfig&);

	void         Reset() override;

	virtual bool AllowOverride() const override { return true; }

private:
	friend class CFlowModuleCallNode;
	CFlowGraphModule* const        m_pModule;

	std::vector<SInputPortConfig>  m_inputs;
	std::vector<SOutputPortConfig> m_outputs;
};

//////////////////////////////////////////////////////////////////////////
// Call Node - the external interface for the module to be called from other graphs
// Dynamically generated by a factory with ports from the Module's interface
//////////////////////////////////////////////////////////////////////////

class CFlowModuleCallNode : public CFlowBaseNode<eNCT_Instanced>
{
public:
	CFlowModuleCallNode(CFlowModuleCallNodeFactory* pFactory, SActivationInfo* pActInfo);
	virtual ~CFlowModuleCallNode();

	virtual IFlowNodePtr Clone(SActivationInfo* pActInfo);
	virtual void         GetMemoryUsage(ICrySizer* s) const { s->Add(*this); }

	virtual void         GetConfiguration(SFlowNodeConfig&);
	virtual void         ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo);

	void                 OnInstanceStarted(TModuleInstanceId instanceId);
	void                 OnInstanceFinished(bool bSuccess, const TModuleParams& params);
	void                 OnInstanceOutput(size_t paramIdx, const TFlowInputData& value);

	uint                 GetId() const { return m_callNodeId; }

private:
	void Serialize(SActivationInfo* pActInfo, TSerialize ser) {}

	enum EInputPort
	{
		eIP_Call = 0,
		eIP_Cancel,
		eIP_IsGlobalController,
		eIP_InstanceId,
		eIP_ContInput,
		eIP_ContOutput,
		eIP_Param1,
		// ...
	};
	enum EOutputPort
	{
		eOP_OnCall = 0,
		eOP_Success,
		eOP_Canceled,
		eOP_Param1,
		// ...
	};
	SActivationInfo                   m_actInfo;

	CFlowModuleCallNodeFactory* const m_pFactory;   //! Reference to the factory that generated this node (has ports info)
	CFlowGraphModule* const           m_pModule;    //! Reference to the Module type for this node
	TModuleId                         m_moduleId;   //! Id of the module type, to check in case the module was deleted
	EntityId                          m_entityId;   //! Entity that this instance is associated with, may be 0 (invalid)
	TModuleInstanceId                 m_instanceId; //! ID of the module instance that this node is associated it
	                                                // Changes at runtime and might be invalid (call nodes exist without an instance before being activated)
	bool                              m_bIsGlobal;
	bool                              m_bEntityChanged;

	const uint                        m_callNodeId;     //! Unique identifier for Call nodes
	static uint                       s_nextCallNodeId; //! Static ID generator for Call node IDs
};

//////////////////////////////////////////////////////////////////////////
//
//	Utility nodes for use with Modules
//
//////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
// helper class to bind user id to a module instance id (e.g. to map entity id to a module id)
//////////////////////////////////////////////////////////////////////////
class CFlowModuleUserIdNode : public CFlowBaseNode<eNCT_Singleton>
{
public:
	CFlowModuleUserIdNode(SActivationInfo* pActInfo) {}
	virtual ~CFlowModuleUserIdNode() {}

	virtual void GetMemoryUsage(ICrySizer* s) const { s->Add(*this); }
	virtual void GetConfiguration(SFlowNodeConfig& config);
	virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo);

private:
	enum EInputPorts
	{
		eIP_Get = 0,
		eIP_Set,
		eIP_UserId,
		eIP_ModuleName,
		eIP_ModuleInstanceId,
	};
	enum EOutputPorts
	{
		eOP_ModuleInstanceId = 0,
	};

	typedef std::unordered_map<string, std::map<int, TModuleInstanceId>, stl::hash_strcmp<string>> TInstanceUserIDs;
	static TInstanceUserIDs s_ids;
};

//////////////////////////////////////////////////////////////////////////
// helper node to get the count of running instances of a module
//////////////////////////////////////////////////////////////////////////
class CFlowModuleCountListener : public CFlowBaseNode<eNCT_Instanced>, IFlowGraphModuleListener
{
public:
	CFlowModuleCountListener(SActivationInfo* pActInfo);
	virtual ~CFlowModuleCountListener();

	virtual IFlowNodePtr Clone(SActivationInfo* pActInfo);
	virtual void         GetMemoryUsage(ICrySizer* s) const { s->Add(*this); }
	virtual void         GetConfiguration(SFlowNodeConfig& config);
	virtual void         ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo);

	//IFlowGraphModuleListener
	virtual void OnModuleInstanceCreated(IFlowGraphModule* pModule, TModuleInstanceId instanceID);
	virtual void OnModuleInstanceDestroyed(IFlowGraphModule* pModule, TModuleInstanceId instanceID);
	virtual void OnModuleDestroyed(IFlowGraphModule* pModule)                                {}
	virtual void OnRootGraphChanged(IFlowGraphModule* module, ERootGraphChangeReason reason) {}
	virtual void OnModulesScannedAndReloaded()                                               {}
	//~IFlowGraphModuleListener

private:
	enum EInputPorts
	{
		eIP_ModuleName,
	};
	enum EOutputPorts
	{
		eOP_InstanceCount,
	};
	SActivationInfo m_actInfo;
	string          m_sModuleName;
};
