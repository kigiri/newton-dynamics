/* Copyright (c) <2003-2016> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#include "dgPhysicsStdafx.h"

#include "dgBody.h"
#include "dgWorld.h"
#include "dgBroadPhase.h"
#include "dgConstraint.h"
#include "dgDynamicBody.h"
#include "dgCollisionInstance.h"
#include "dgSkeletonContainer.h"
#include "dgWorldDynamicUpdate.h"
#include "dgBilateralConstraint.h"
#include "dgCollisionDeformableMesh.h"

#define DG_CCD_EXTRA_CONTACT_COUNT			(8 * 3)
#define DG_PARALLEL_JOINT_COUNT_CUT_OFF		(256)

dgVector dgWorldDynamicUpdate::m_velocTol (dgFloat32 (1.0e-8f));


class dgWorldDynamicUpdateSyncDescriptor
{
	public:
	dgWorldDynamicUpdateSyncDescriptor()
	{
		memset (this, 0, sizeof (dgWorldDynamicUpdateSyncDescriptor));
	}

	dgFloat32 m_timestep;
	dgInt32 m_atomicCounter;
	
	dgInt32 m_clusterCount;
	dgInt32 m_firstCluster;
	dgThread::dgCriticalSection* m_criticalSection;
};


void dgJacobianMemory::Init(dgWorld* const world, dgInt32 rowsCount, dgInt32 bodyCount, dgInt32 blockMatrixSizeInBytes)
{
	world->m_solverJacobiansMemory.ResizeIfNecessary((rowsCount + 1) * sizeof(dgJacobianMatrixElement));
	m_jacobianBuffer = (dgJacobianMatrixElement*)&world->m_solverJacobiansMemory[0];

	world->m_solverForceAccumulatorMemory.ResizeIfNecessary((bodyCount + 8) * sizeof(dgJacobian));
	m_internalForcesBuffer = (dgJacobian*)&world->m_solverForceAccumulatorMemory[0];
	dgAssert(bodyCount <= (((world->m_solverForceAccumulatorMemory.GetBytesCapacity() - 16) / dgInt32(sizeof(dgJacobian))) & (-8)));

	dgAssert((dgUnsigned64(m_jacobianBuffer) & 0x01f) == 0);
	dgAssert((dgUnsigned64(m_internalForcesBuffer) & 0x01f) == 0);
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

dgWorldDynamicUpdate::dgWorldDynamicUpdate()
	:m_bodies(0)
	,m_joints(0)
	,m_clusters(0)
	,m_markLru(0)
	,m_softBodyCriticalSectionLock()
	,m_clusterMemory(NULL)
{
}

void dgWorldDynamicUpdate::UpdateDynamics(dgFloat32 timestep)
{
	dgWorld* const world = (dgWorld*) this;

	m_bodies = 0;
	m_joints = 0;
	m_clusters = 0;
	world->m_dynamicsLru = world->m_dynamicsLru + DG_BODY_LRU_STEP;
	m_markLru = world->m_dynamicsLru;

	dgDynamicBody* const sentinelBody = world->m_sentinelBody;
	sentinelBody->m_index = 0; 
	sentinelBody->m_resting = 1;
	sentinelBody->m_sleeping = 1;
	sentinelBody->m_equilibrium = 1;
	sentinelBody->m_dynamicsLru = m_markLru;

	BuildClusters(timestep);
	SortClustersByCount();

	dgInt32 maxRowCount = 0;
	dgInt32 blockMatrixSize = 0;
	dgInt32 softBodiesCount = 0;
	for (dgInt32 i = 0; i < m_clusters; i ++) {
		dgBodyCluster& cluster = m_clusterMemory[i];
		cluster.m_rowsStart = maxRowCount;
		maxRowCount += cluster.m_rowsCount;
		softBodiesCount += cluster.m_hasSoftBodies;
	}
	m_solverMemory.Init (world, maxRowCount, m_bodies, blockMatrixSize);

	dgInt32 threadCount = world->GetThreadCount();	

	dgWorldDynamicUpdateSyncDescriptor descriptor;
	descriptor.m_timestep = timestep;

	dgInt32 index = softBodiesCount;

	descriptor.m_atomicCounter = 0;
	descriptor.m_firstCluster = index;
	descriptor.m_clusterCount = m_clusters - index;

	dgInt32 useParallel = world->m_useParallelSolver && (threadCount > 1);
	//useParallel = 1;
	if (useParallel) {
		dgInt32 sum = m_joints;
		useParallel = useParallel && m_joints && m_clusters;
		useParallel = useParallel && ((threadCount * m_clusterMemory[0].m_jointCount) >= sum);
		useParallel = useParallel && (m_clusterMemory[0].m_jointCount > DG_PARALLEL_JOINT_COUNT_CUT_OFF);
		//useParallel = 1;
		while (useParallel) {
			CalculateReactionForcesParallel(&m_clusterMemory[index], timestep);
			index ++;
			sum -= m_clusterMemory[index].m_jointCount;
			useParallel = useParallel && (index < m_clusters);
			useParallel = useParallel && ((threadCount * m_clusterMemory[index].m_jointCount) >= m_joints);
			useParallel = useParallel && (m_clusterMemory[index].m_jointCount > DG_PARALLEL_JOINT_COUNT_CUT_OFF);
		}
	}

	if (index < m_clusters) {
		descriptor.m_atomicCounter = 0;
		descriptor.m_firstCluster = index;
		descriptor.m_clusterCount = m_clusters - index;
		for (dgInt32 i = 0; i < threadCount; i ++) {
			world->QueueJob (CalculateClusterReactionForcesKernel, &descriptor, world);
		}
		world->SynchronizationBarrier();
	}

	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*)&world->m_bodiesMemory[0];
	for (dgInt32 i = 0; i < softBodiesCount; i++) {
		dgBodyCluster* const cluster = &m_clusterMemory[i];
//		IntegrateExternalForce(cluster, timestep, 0);
		dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart];
		dgAssert (cluster->m_bodyCount == 2);
		dgDynamicBody* const body = (dgDynamicBody*)bodyArray[1].m_body;
		dgAssert (body->m_collision->IsType(dgCollision::dgCollisionLumpedMass_RTTI));
		body->IntegrateOpenLoopExternalForce(timestep);
		IntegrateVelocity(cluster, DG_SOLVER_MAX_ERROR, timestep, 0);
	}

	m_clusterMemory = NULL;
}

void dgWorldDynamicUpdate::SortClustersByCount ()
{
	dgSort(m_clusterMemory, m_clusters, CompareClusters);
}

void dgWorldDynamicUpdate::BuildClusters(dgFloat32 timestep)
{
	dgWorld* const world = (dgWorld*) this;
	dgUnsigned32 lru = m_markLru - 1;

	dgBodyMasterList& masterList = *world;

	dgAssert (masterList.GetFirst()->GetInfo().GetBody() == world->m_sentinelBody);
	world->m_solverJacobiansMemory.ResizeIfNecessary ((2 * (masterList.m_constraintCount + 1024)) * sizeof (dgDynamicBody*));
	dgDynamicBody** const stackPoolBuffer = (dgDynamicBody**)&world->m_solverJacobiansMemory[0];

	for (dgBodyMasterList::dgListNode* node = masterList.GetLast(); node; node = node->GetPrev()) {
		const dgBodyMasterListRow& graphNode = node->GetInfo();
		dgBody* const body = graphNode.GetBody();
		
		if (body->GetInvMass().m_w == dgFloat32(0.0f)) {
#ifdef _DEBUG
			for (; node; node = node->GetPrev()) {
				//dgAssert ((body->GetType() == dgBody::m_kinamticBody) ||(node->GetInfo().GetBody()->GetInvMass().m_w == dgFloat32(0.0f)));
				dgAssert(node->GetInfo().GetBody()->GetInvMass().m_w == dgFloat32(0.0f));
			}
#endif
			break;
		}

		if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
			dgDynamicBody* const dynamicBody = (dgDynamicBody*)body;
			if (dynamicBody->m_dynamicsLru < lru) {
				if (!(dynamicBody->m_freeze | dynamicBody->m_spawnnedFromCallback | dynamicBody->m_sleeping)) {
					SpanningTree(dynamicBody, stackPoolBuffer, timestep);
				}
			}
			dynamicBody->m_spawnnedFromCallback = false;
		}
	}
}

void dgWorldDynamicUpdate::SpanningTree (dgDynamicBody* const body, dgDynamicBody** const queueBuffer, dgFloat32 timestep)
{
	dgInt32 stack = 1;
	dgInt32 bodyCount = 1;
	dgInt32 jointCount = 0;
	dgInt32 hasSoftBodies = 0;
	dgInt32 isInEquilibrium = 1;

	dgWorld* const world = (dgWorld*) this;
	const dgInt32 clusterLRU = world->m_clusterLRU;
	const dgUnsigned32 lruMark = m_markLru - 1;

	world->m_clusterLRU ++;

	queueBuffer[0] = body;
	world->m_bodiesMemory.ResizeIfNecessary ((m_bodies + 1) * sizeof (dgBodyInfo));
	dgBodyInfo* const bodyArray0 = (dgBodyInfo*)&world->m_bodiesMemory[0];

	bodyArray0[m_bodies].m_body = world->m_sentinelBody;
	dgAssert(world->m_sentinelBody->m_index == 0);
	dgAssert(dgInt32(world->m_sentinelBody->m_dynamicsLru) == m_markLru);
	const dgInt32 vectorStride = dgInt32 (sizeof (dgVector) / sizeof (dgFloat32));

	bool globalAutoSleep = true;
	while (stack) {
		stack --;
		dgDynamicBody* const srcBody = queueBuffer[stack];

		if (srcBody->m_dynamicsLru < lruMark) {
//hack
//srcBody->m_equilibrium = false;

			dgAssert(srcBody->GetInvMass().m_w > dgFloat32(0.0f));
			dgAssert(srcBody->m_masterNode);

			dgInt32 bodyIndex = m_bodies + bodyCount;
			world->m_bodiesMemory.ResizeIfNecessary ((bodyIndex + 1) * sizeof (dgBodyInfo));
			dgBodyInfo* const bodyArray1 = (dgBodyInfo*)&world->m_bodiesMemory[0];
			bodyArray1[bodyIndex].m_body = srcBody;
			isInEquilibrium &= srcBody->m_equilibrium;
			globalAutoSleep &= (srcBody->m_autoSleep & srcBody->m_equilibrium); 
			
			srcBody->m_index = bodyCount;
			srcBody->m_dynamicsLru = lruMark;
			srcBody->m_resting = srcBody->m_equilibrium;

			hasSoftBodies |= (srcBody->m_collision->IsType(dgCollision::dgCollisionDeformableMesh_RTTI) ? 1 : 0);

			srcBody->m_sleeping = false;

			bodyCount++;
			for (dgBodyMasterListRow::dgListNode* jointNode = srcBody->m_masterNode->GetInfo().GetFirst(); jointNode; jointNode = jointNode->GetNext()) {

				dgBodyMasterListCell* const cell = &jointNode->GetInfo();
				dgConstraint* const constraint = cell->m_joint;
				dgAssert(constraint);
				dgBody* const linkBody = cell->m_bodyNode;
				dgAssert((constraint->m_body0 == srcBody) || (constraint->m_body1 == srcBody));
				dgAssert((constraint->m_body0 == linkBody) || (constraint->m_body1 == linkBody));
				const dgContact* const contact = (constraint->GetId() == dgConstraint::m_contactConstraint) ? (dgContact*)constraint : NULL;

				bool check0 = linkBody->IsCollidable();
				check0 = check0 && (!contact || (contact->m_contactActive && contact->m_maxDOF) || (srcBody->m_continueCollisionMode | linkBody->m_continueCollisionMode));
				if (check0) {
					bool check1 = constraint->m_dynamicsLru != lruMark;
					if (check1) {
						const dgInt32 jointIndex = m_joints + jointCount;
						world->m_jointsMemory.ResizeIfNecessary ((jointIndex + 1) * sizeof (dgJointInfo));
						dgJointInfo* const constraintArray = (dgJointInfo*)&world->m_jointsMemory[0];

						constraint->m_index = jointCount;
						constraint->m_clusterLRU = clusterLRU;
						constraint->m_dynamicsLru = lruMark;

						constraintArray[jointIndex].m_joint = constraint;
						const dgInt32 rows = (constraint->m_maxDOF + vectorStride - 1) & (-vectorStride);
						constraintArray[jointIndex].m_pairCount = dgInt16(rows);
						jointCount++;

						dgAssert(constraint->m_body0);
						dgAssert(constraint->m_body1);
					}

					dgDynamicBody* const adjacentBody = (dgDynamicBody*)linkBody;
					if ((adjacentBody->m_dynamicsLru != lruMark) && (adjacentBody->GetInvMass().m_w > dgFloat32(0.0f))) {
						queueBuffer[stack] = adjacentBody;
						stack ++;
					}
				}
			}
		}
	}


	dgBodyInfo* const bodyArray = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	if (globalAutoSleep) {
		for (dgInt32 i = 1; i < bodyCount; i++) {
			dgBody* const body1 = bodyArray[m_bodies + i].m_body;
			body1->m_dynamicsLru = m_markLru;
			body1->m_sleeping = globalAutoSleep;
		}
	} else {
		if (world->m_clusterUpdate) {
			dgClusterCallbackStruct record;
			record.m_world = world;
			record.m_count = bodyCount;
			record.m_strideInByte = sizeof (dgBodyInfo);
			record.m_bodyArray = &bodyArray[m_bodies].m_body;
			if (!world->m_clusterUpdate(world, &record, bodyCount)) {
				for (dgInt32 i = 0; i < bodyCount; i++) {
					dgBody* const body1 = bodyArray[m_bodies + i].m_body;
					body1->m_dynamicsLru = m_markLru;
				}
				return;
			}
		}

		world->m_clusterMemory.ResizeIfNecessary  ((m_clusters + 1) * sizeof (dgBodyCluster));
		m_clusterMemory = (dgBodyCluster*) &world->m_clusterMemory[0];
		dgBodyCluster& cluster = m_clusterMemory[m_clusters];

		cluster.m_bodyStart = m_bodies;
		cluster.m_jointStart = m_joints;
		cluster.m_bodyCount = bodyCount;
		cluster.m_clusterLRU = clusterLRU;
		cluster.m_jointCount = jointCount;
		
		cluster.m_rowsStart = 0;
		cluster.m_isContinueCollision = 0;
		cluster.m_hasSoftBodies = dgInt16 (hasSoftBodies);

		dgJointInfo* const constraintArrayPtr = (dgJointInfo*)&world->m_jointsMemory[0];
		dgJointInfo* const constraintArray = &constraintArrayPtr[m_joints];

		dgInt32 rowsCount = 0;
		dgInt32 isContinueCollisionCluster = 0;
		for (dgInt32 i = 0; i < jointCount; i++) {
			dgJointInfo* const jointInfo = &constraintArray[i];
			dgConstraint* const joint = jointInfo->m_joint;

			dgBody* const body0 = joint->m_body0;
			dgBody* const body1 = joint->m_body1;

			dgInt32 m0 = (body0->GetInvMass().m_w != dgFloat32(0.0f)) ? body0->m_index : 0;
			dgInt32 m1 = (body1->GetInvMass().m_w != dgFloat32(0.0f)) ? body1->m_index : 0;

			jointInfo->m_m0 = m0;
			jointInfo->m_m1 = m1;

			body0->m_dynamicsLru = m_markLru;
			body1->m_dynamicsLru = m_markLru;

			dgAssert (constraintArray[i].m_pairCount >= 0);
			dgAssert (constraintArray[i].m_pairCount < 64);
			rowsCount += constraintArray[i].m_pairCount;
			if (joint->GetId() == dgConstraint::m_contactConstraint) {
				if (body0->m_continueCollisionMode | body1->m_continueCollisionMode) {
					dgInt32 ccdJoint = false;
					const dgVector& veloc0 = body0->m_veloc;
					const dgVector& veloc1 = body1->m_veloc;

					const dgVector& omega0 = body0->m_omega;
					const dgVector& omega1 = body1->m_omega;

					const dgVector& com0 = body0->m_globalCentreOfMass;
					const dgVector& com1 = body1->m_globalCentreOfMass;

					const dgCollisionInstance* const collision0 = body0->m_collision;
					const dgCollisionInstance* const collision1 = body1->m_collision;
					dgFloat32 dist = dgMax(body0->m_collision->GetBoxMinRadius(), body1->m_collision->GetBoxMinRadius()) * dgFloat32(0.25f);

					dgVector relVeloc(veloc1 - veloc0);
					dgVector relOmega(omega1 - omega0);
					dgVector relVelocMag2(relVeloc.DotProduct4(relVeloc));
					dgVector relOmegaMag2(relOmega.DotProduct4(relOmega));

					if ((relOmegaMag2.m_w > dgFloat32(1.0f)) || ((relVelocMag2.m_w * timestep * timestep) > (dist * dist))) {
						dgTriplex normals[16];
						dgTriplex points[16];
						dgInt64 attrib0[16];
						dgInt64 attrib1[16];
						dgFloat32 penetrations[16];
						dgFloat32 timeToImpact = timestep;
						const dgInt32 ccdContactCount = world->CollideContinue(collision0, body0->m_matrix, veloc0, omega0, collision1, body1->m_matrix, veloc1, omega1,
																			   timeToImpact, points, normals, penetrations, attrib0, attrib1, 6, 0);

						for (dgInt32 j = 0; j < ccdContactCount; j++) {
							dgVector point(&points[j].m_x);
							dgVector normal(&normals[j].m_x);
							dgVector vel0(veloc0 + omega0.CrossProduct3(point - com0));
							dgVector vel1(veloc1 + omega1.CrossProduct3(point - com1));
							dgVector vRel(vel1 - vel0);
							dgFloat32 contactDistTravel = vRel.DotProduct4(normal).m_w * timestep;
							ccdJoint |= (contactDistTravel > dist);
						}
					}
					//ccdJoint = body0->m_continueCollisionMode | body1->m_continueCollisionMode;
					isContinueCollisionCluster |= ccdJoint;
					rowsCount += DG_CCD_EXTRA_CONTACT_COUNT;
				}
			}
		}

		if (isContinueCollisionCluster) {
			rowsCount = dgMax(rowsCount, 64);
		}
		cluster.m_rowsCount = rowsCount;
		cluster.m_isContinueCollision = dgInt16 (isContinueCollisionCluster);

		m_clusters++;
		m_bodies += bodyCount;
		m_joints += jointCount;
	}
}


dgInt32 dgWorldDynamicUpdate::SortClusters(const dgBodyCluster* const cluster, dgFloat32 timestep, dgInt32 threadID) const
{
	dgWorld* const world = (dgWorld*) this;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*)&world->m_bodiesMemory[0];
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*)&world->m_jointsMemory[0];
	
	dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart];
	dgJointInfo* const constraintArray = &constraintArrayPtr[cluster->m_jointStart];

	dgJointInfo* const tmpInfoList = dgAlloca(dgJointInfo, cluster->m_jointCount);
	dgJointInfo** queueBuffer = dgAlloca(dgJointInfo*, cluster->m_jointCount * 2 + 1024 * 8);
	dgQueue<dgJointInfo*> queue(queueBuffer, cluster->m_jointCount * 2 + 1024 * 8);
	dgFloat32 heaviestMass = dgFloat32(1.0e20f);
	dgInt32 infoIndex = 0;
	dgInt32 activeJoints = 0;
	const dgInt32 lru = cluster->m_clusterLRU;
	dgJointInfo* heaviestBody = NULL;

	const dgInt32 jointCount = cluster->m_jointCount;
	for (dgInt32 i = 0; i < jointCount; i++) {
		dgJointInfo& jointInfo = constraintArray[i];
		tmpInfoList[i] = jointInfo;
		tmpInfoList[i].m_scale0 = dgFloat32 (0.0f);

		const dgInt32 m0 = jointInfo.m_m0;
		const dgInt32 m1 = jointInfo.m_m1;
		dgBody* const body0 = bodyArray[m0].m_body;
		dgBody* const body1 = bodyArray[m1].m_body;

		const dgFloat32 invMass0 = body0->GetInvMass().m_w;
		const dgFloat32 invMass1 = body1->GetInvMass().m_w;

		dgInt32 resting = body0->m_equilibrium & body1->m_equilibrium;
		body0->m_resting &= resting | (invMass0 == dgFloat32 (0.0f));
		body1->m_resting &= resting | (invMass1 == dgFloat32 (0.0f));

		if ((invMass0 == dgFloat32 (0.0f)) || (invMass1 == dgFloat32 (0.0f))) {
			queue.Insert(&tmpInfoList[i]);
			tmpInfoList[i].m_scale0 = dgFloat32 (1.0f);
		} else if (invMass0 && (heaviestMass > invMass0)) {
			heaviestMass = invMass0;
			heaviestBody = &tmpInfoList[i];
		} else if (invMass1 && (heaviestMass > invMass1)) {
			heaviestMass = invMass1;
			heaviestBody = &tmpInfoList[i];
		}
	}

	if (queue.IsEmpty()) {
		dgAssert(heaviestBody);
		queue.Insert(heaviestBody);
		heaviestBody->m_scale0 = dgFloat32 (1.0f);
	}

	while (!queue.IsEmpty()) {
		dgInt32 count = queue.m_firstIndex - queue.m_lastIndex;
		if (count < 0) {
			count += queue.m_mod;
		}

		dgInt32 index = queue.m_lastIndex;
		queue.Reset();

		for (dgInt32 j = 0; j < count; j++) {
			dgJointInfo* const jointInfo = queue.m_pool[index];
			dgConstraint* const constraint = jointInfo->m_joint;
			if (constraint->m_clusterLRU == lru) {
				dgAssert (dgInt32 (constraint->m_index) < cluster->m_jointCount);
				constraint->m_index = infoIndex;
				constraintArray[infoIndex] = *jointInfo;
				constraint->m_clusterLRU--;
				infoIndex++;
				dgAssert(infoIndex <= cluster->m_jointCount);

				const dgInt32 m0 = jointInfo->m_m0;
				const dgInt32 m1 = jointInfo->m_m1;
				const dgBody* const body0 = bodyArray[m0].m_body;
				const dgBody* const body1 = bodyArray[m1].m_body;
				
				activeJoints += !(body0->m_resting & body1->m_resting);
				
				if (body0->GetInvMass().m_w > dgFloat32(0.0f)) {
					for (dgBodyMasterListRow::dgListNode* jointNode1 = body0->m_masterNode->GetInfo().GetFirst(); jointNode1; jointNode1 = jointNode1->GetNext()) {
						dgBodyMasterListCell* const cell1 = &jointNode1->GetInfo();
						dgConstraint* const constraint1 = cell1->m_joint;
						if (constraint1->m_clusterLRU == lru) {
							dgJointInfo* const nextInfo = &tmpInfoList[constraint1->m_index];
							if (!nextInfo->m_scale0) {
								queue.Insert(nextInfo);
								nextInfo->m_scale0 = dgFloat32 (1.0f);
							}
						}
					}
				}

				if (body1->GetInvMass().m_w > dgFloat32(0.0f)) {
					for (dgBodyMasterListRow::dgListNode* jointNode1 = body1->m_masterNode->GetInfo().GetFirst(); jointNode1; jointNode1 = jointNode1->GetNext()) {
						dgBodyMasterListCell* const cell1 = &jointNode1->GetInfo();
						dgConstraint* const constraint1 = cell1->m_joint;

						if (constraint1->m_clusterLRU == lru){
							dgJointInfo* const nextInfo = &tmpInfoList[constraint1->m_index];
							if (!nextInfo->m_scale0) {
								queue.Insert(nextInfo);
								nextInfo->m_scale0 = dgFloat32 (1.0f);
							}
						}
					}
				}

				if (infoIndex == cluster->m_jointCount) {
					queue.Reset();
					break;
				}
			}
			index++;
			if (index >= queue.m_mod) {
				index = 0;
			}
		}
	}

	dgAssert(infoIndex == cluster->m_jointCount);
	return activeJoints;
}


dgBody* dgWorldDynamicUpdate::GetClusterBody(const void* const clusterPtr, dgInt32 index) const
{
	const dgClusterCallbackStruct* const cluster = (dgClusterCallbackStruct*)clusterPtr;

	char* const ptr = &((char*)cluster->m_bodyArray)[cluster->m_strideInByte * index];
	dgBody** const bodyPtr = (dgBody**)ptr;
	return (index < cluster->m_count) ? ((index >= 0) ? *bodyPtr : NULL) : NULL;
}


// sort from high to low
dgInt32 dgWorldDynamicUpdate::CompareClusters(const dgBodyCluster* const clusterA, const dgBodyCluster* const clusterB, void* notUsed)
{
	dgInt32 countA = clusterA->m_jointCount + (clusterA->m_hasSoftBodies << 30);
	dgInt32 countB = clusterB->m_jointCount + (clusterB->m_hasSoftBodies << 30);

	if (countA < countB) {
		return 1;
	}
	if (countA > countB) {
		return -1;
	}
	return 0;
}


void dgWorldDynamicUpdate::CalculateClusterReactionForcesKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgWorldDynamicUpdateSyncDescriptor* const descriptor = (dgWorldDynamicUpdateSyncDescriptor*) context;

	dgFloat32 timestep = descriptor->m_timestep;
	dgWorld* const world = (dgWorld*) worldContext;
	dgInt32 count = descriptor->m_clusterCount;
	dgBodyCluster* const clusters = &((dgBodyCluster*)&world->m_clusterMemory[0])[descriptor->m_firstCluster];

	for (dgInt32 i = dgAtomicExchangeAndAdd(&descriptor->m_atomicCounter, 1); i < count; i = dgAtomicExchangeAndAdd(&descriptor->m_atomicCounter, 1)) {
		dgBodyCluster* const cluster = &clusters[i]; 
		world->ResolveClusterForces (cluster, threadID, timestep);
	}
}

dgInt32 dgWorldDynamicUpdate::GetJacobianDerivatives (dgContraintDescritor& constraintParamOut, dgJointInfo* const jointInfo, dgConstraint* const constraint, dgJacobianMatrixElement* const matrixRow, dgInt32 rowCount) const
{
	dgInt32 dof = dgInt32(constraint->m_maxDOF);
	dgAssert(dof <= DG_CONSTRAINT_MAX_ROWS);
	for (dgInt32 i = 0; i < dof; i++) {
		constraintParamOut.m_forceBounds[i].m_low = DG_MIN_BOUND;
		constraintParamOut.m_forceBounds[i].m_upper = DG_MAX_BOUND;
		constraintParamOut.m_forceBounds[i].m_jointForce = NULL;
		constraintParamOut.m_forceBounds[i].m_normalIndex = DG_NORMAL_CONSTRAINT;
	}

	dgAssert(constraint->m_body0);
	dgAssert(constraint->m_body1);

	dgBody* const body0 = constraint->m_body0;
	dgBody* const body1 = constraint->m_body1;

	dgAssert(body0->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body0->IsRTTIType(dgBody::m_kinematicBodyRTTI));
	dgAssert(body1->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body1->IsRTTIType(dgBody::m_kinematicBodyRTTI));

	body0->m_inCallback = true;
	body1->m_inCallback = true;
	dof = constraint->JacobianDerivative(constraintParamOut);
	body0->m_inCallback = false;
	body1->m_inCallback = false;

	if (constraint->GetId() == dgConstraint::m_contactConstraint) {
		dgSkeletonContainer* const skeleton0 = body0->GetSkeleton();
		dgSkeletonContainer* const skeleton1 = body1->GetSkeleton();
		if (skeleton0 && (skeleton0 == skeleton1)) {
			skeleton0->AddSelfCollisionJoint((dgContact*)constraint);
		} else if (skeleton0 && !skeleton1) {
//			skeleton0->AddSelfCollisionJoint((dgContact*)constraint);
		} else if (skeleton1 && !skeleton0) {
//			skeleton1->AddSelfCollisionJoint((dgContact*)constraint);
		}
	}
	
	jointInfo->m_pairCount = dof;
	jointInfo->m_pairStart = rowCount;
	for (dgInt32 i = 0; i < dof; i++) {
		dgJacobianMatrixElement* const row = &matrixRow[rowCount];
		dgAssert(constraintParamOut.m_forceBounds[i].m_jointForce);
		row->m_Jt = constraintParamOut.m_jacobian[i];

		row->m_diagDamp = dgFloat32 (0.0f);
		row->m_stiffness = dgMax (DG_PSD_DAMP_TOL * (dgFloat32 (1.0f) - constraintParamOut.m_jointStiffness[i]), dgFloat32 (1.0e-5f));
		dgAssert(row->m_stiffness >= dgFloat32(0.0f));
		dgAssert(constraintParamOut.m_jointStiffness[i] <= dgFloat32(1.0f));
		dgAssert ((dgFloat32 (1.0f) - constraintParamOut.m_jointStiffness[i]) >= dgFloat32 (0.0f));
		row->m_coordenateAccel = constraintParamOut.m_jointAccel[i];
		row->m_restitution = constraintParamOut.m_restitution[i];
		row->m_penetration = constraintParamOut.m_penetration[i];
		row->m_penetrationStiffness = constraintParamOut.m_penetrationStiffness[i];
		row->m_lowerBoundFrictionCoefficent = constraintParamOut.m_forceBounds[i].m_low;
		row->m_upperBoundFrictionCoefficent = constraintParamOut.m_forceBounds[i].m_upper;
		row->m_jointFeebackForce = constraintParamOut.m_forceBounds[i].m_jointForce;

		dgInt32 frictionIndex = constraintParamOut.m_forceBounds[i].m_normalIndex < 0 ? dof : constraintParamOut.m_forceBounds[i].m_normalIndex;
		row->m_normalForceIndex = frictionIndex;
		rowCount++;
	}
	rowCount = (rowCount & (dgInt32(sizeof (dgVector) / sizeof (dgFloat32)) - 1)) ? ((rowCount & (-dgInt32(sizeof (dgVector) / sizeof (dgFloat32)))) + dgInt32(sizeof (dgVector) / sizeof (dgFloat32))) : rowCount;
	dgAssert((rowCount & (dgInt32(sizeof (dgVector) / sizeof (dgFloat32)) - 1)) == 0);

	constraint->ResetInverseDynamics();

	return rowCount;
}


void dgWorldDynamicUpdate::IntegrateVelocity(const dgBodyCluster* const cluster, dgFloat32 accelTolerance, dgFloat32 timestep, dgInt32 threadID) const
{
#if 0
	bool stackSleeping = true;
	bool isClusterResting = true;
	dgInt32 sleepCounter = 10000;

	dgWorld* const world = (dgWorld*) this;

	dgFloat32 velocityDragCoeff = DG_FREEZZING_VELOCITY_DRAG;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart + 1]; 
	dgInt32 count = cluster->m_bodyCount - 1;
	if (count <= 2) {
		//bool autosleep = bodyArray[0].m_body->m_autoSleep;
		bool equilibrium  = bodyArray[0].m_body->m_equilibrium;
		if (count == 2) {
			equilibrium  &= bodyArray[1].m_body->m_equilibrium;
		}
		if (!equilibrium ) {
			velocityDragCoeff = dgFloat32 (0.9999f);
		}
	}

	dgFloat32 maxAccel = dgFloat32 (0.0f);
	dgFloat32 maxAlpha = dgFloat32 (0.0f);
	dgFloat32 maxSpeed = dgFloat32 (0.0f);
	dgFloat32 maxOmega = dgFloat32 (0.0f);

	const dgFloat32 speedFreeze = world->m_freezeSpeed2;
	const dgFloat32 accelFreeze = world->m_freezeAccel2 * ((cluster->m_jointCount <= DG_SMALL_ISLAND_COUNT) ? dgFloat32 (0.05f) : dgFloat32 (1.0f));
	dgVector velocDragVect (velocityDragCoeff, velocityDragCoeff, velocityDragCoeff, dgFloat32 (0.0f));

	for (dgInt32 i = 0; i < count; i ++) {
		dgBody* const body = bodyArray[i].m_body;
		dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBody));
		
		body->m_equilibrium = 1;
		dgVector isMovingMask ((body->m_veloc + body->m_omega + body->m_accel + body->m_alpha) & dgVector::m_signMask);
		if ((isMovingMask.TestZero().GetSignMask() & 7) != 7) {
			dgAssert (body->m_invMass.m_w);
			if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
				body->IntegrateVelocity(timestep);
			}

			dgAssert (body->m_accel.m_w == dgFloat32 (0.0f));
			dgAssert (body->m_alpha.m_w == dgFloat32 (0.0f));
			dgAssert (body->m_veloc.m_w == dgFloat32 (0.0f));
			dgAssert (body->m_omega.m_w == dgFloat32 (0.0f));
			dgFloat32 accel2 = body->m_accel.DotProduct4(body->m_accel).GetScalar();
			dgFloat32 alpha2 = body->m_alpha.DotProduct4(body->m_alpha).GetScalar();
			dgFloat32 speed2 = body->m_veloc.DotProduct4(body->m_veloc).GetScalar();
			dgFloat32 omega2 = body->m_omega.DotProduct4(body->m_omega).GetScalar();

			maxAccel = dgMax (maxAccel, accel2);
			maxAlpha = dgMax (maxAlpha, alpha2);
			maxSpeed = dgMax (maxSpeed, speed2);
			maxOmega = dgMax (maxOmega, omega2);
			bool equilibrium = (accel2 < accelFreeze) && (alpha2 < accelFreeze) && (speed2 < speedFreeze) && (omega2 < speedFreeze);
			if (equilibrium) {
				dgVector veloc (body->m_veloc * velocDragVect);
				dgVector omega (body->m_omega * velocDragVect);
				body->m_veloc = (veloc.DotProduct4(veloc) > m_velocTol) & veloc;
				body->m_omega = (omega.DotProduct4(omega) > m_velocTol) & omega;
			}

			body->m_equilibrium = equilibrium ? 1 : 0;
//body->m_equilibrium &= body->m_autoSleep;
			stackSleeping &= equilibrium;
			isClusterResting &= (body->m_autoSleep & equilibrium);
			if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
				sleepCounter = dgMin (sleepCounter, ((dgDynamicBody*)body)->m_sleepingCounter);
			}

			body->UpdateCollisionMatrix (timestep, threadID);
		}
	}

	if (isClusterResting && cluster->m_jointCount) {
		if (stackSleeping) {
			for (dgInt32 i = 0; i < count; i ++) {
				dgBody* const body =  bodyArray[i].m_body;
				dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
				body->m_accel = dgVector::m_zero;
				body->m_alpha = dgVector::m_zero;
				body->m_veloc = dgVector::m_zero;
				body->m_omega = dgVector::m_zero;
				body->m_sleeping = body->m_autoSleep;
			}
		} else {
			const bool state = (maxAccel > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAccel) || (maxAlpha > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAlpha) ||
							   (maxSpeed > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxVeloc) || (maxOmega > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxOmega);
			if (state) { 
					for (dgInt32 i = 0; i < count; i ++) {
						dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
						dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI));
						body->m_sleepingCounter = 0;
					}
			} else {
				dgInt32 index = 0;
				for (dgInt32 i = 0; i < DG_SLEEP_ENTRIES; i ++) {
					if ((maxAccel <= world->m_sleepTable[i].m_maxAccel) &&
						(maxAlpha <= world->m_sleepTable[i].m_maxAlpha) &&
						(maxSpeed <= world->m_sleepTable[i].m_maxVeloc) &&
						(maxOmega <= world->m_sleepTable[i].m_maxOmega)) {
							index = i;
							break;
					}
				}

				dgInt32 timeScaleSleepCount = dgInt32 (dgFloat32 (60.0f) * sleepCounter * timestep);
				if (timeScaleSleepCount > world->m_sleepTable[index].m_steps) {
					for (dgInt32 i = 0; i < count; i ++) {
						dgBody* const body = bodyArray[i].m_body;
						dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
						body->m_accel = dgVector::m_zero;
						body->m_alpha = dgVector::m_zero;
						body->m_veloc = dgVector::m_zero;
						body->m_omega = dgVector::m_zero;
						body->m_equilibrium = 1;
						body->m_sleeping = body->m_autoSleep;
					}
				} else {
					sleepCounter ++;
					for (dgInt32 i = 0; i < count; i ++) {
						dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
						if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
							body->m_sleepingCounter = sleepCounter;
						}
					}
				}
			}
		}
	}
#else

	dgWorld* const world = (dgWorld*) this;
	dgFloat32 velocityDragCoeff = DG_FREEZZING_VELOCITY_DRAG;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*)&world->m_bodiesMemory[0];
	dgBodyInfo* const bodyArray = &bodyArrayPtr[cluster->m_bodyStart + 1];
	dgInt32 count = cluster->m_bodyCount - 1;
	if (count <= 2) {
		//bool autosleep = bodyArray[0].m_body->m_autoSleep;
		bool equilibrium = bodyArray[0].m_body->m_equilibrium;
		if (count == 2) {
			equilibrium &= bodyArray[1].m_body->m_equilibrium;
		}
		if (!equilibrium) {
			velocityDragCoeff = dgFloat32(0.9999f);
		}
	}

	dgFloat32 maxAccel = dgFloat32(0.0f);
	dgFloat32 maxAlpha = dgFloat32(0.0f);
	dgFloat32 maxSpeed = dgFloat32(0.0f);
	dgFloat32 maxOmega = dgFloat32(0.0f);

	const dgFloat32 speedFreeze = world->m_freezeSpeed2;
	const dgFloat32 accelFreeze = world->m_freezeAccel2 * ((cluster->m_jointCount <= DG_SMALL_ISLAND_COUNT) ? dgFloat32(0.05f) : dgFloat32(1.0f));
	dgVector velocDragVect(velocityDragCoeff, velocityDragCoeff, velocityDragCoeff, dgFloat32(0.0f));

//static int xxxxx;
//xxxxx++;
	bool stackSleeping = true;
	//bool isClusterResting = true;
	dgInt32 sleepCounter = 10000;
	for (dgInt32 i = 0; i < count; i++) {
		dgBody* const body = bodyArray[i].m_body;
		dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBody));

		body->m_equilibrium = 1;
		//dgVector isMovingMask((body->m_veloc + body->m_omega + body->m_accel + body->m_alpha) & dgVector::m_signMask);
		dgVector isMovingMask(body->m_veloc + body->m_omega + body->m_accel + body->m_alpha);
		if ((isMovingMask.TestZero().GetSignMask() & 7) != 7) {
			dgAssert(body->m_invMass.m_w);
			if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
				body->IntegrateVelocity(timestep);
			}

			dgAssert(body->m_accel.m_w == dgFloat32(0.0f));
			dgAssert(body->m_alpha.m_w == dgFloat32(0.0f));
			dgAssert(body->m_veloc.m_w == dgFloat32(0.0f));
			dgAssert(body->m_omega.m_w == dgFloat32(0.0f));
			dgFloat32 accel2 = body->m_accel.DotProduct4(body->m_accel).GetScalar();
			dgFloat32 alpha2 = body->m_alpha.DotProduct4(body->m_alpha).GetScalar();
			dgFloat32 speed2 = body->m_veloc.DotProduct4(body->m_veloc).GetScalar();
			dgFloat32 omega2 = body->m_omega.DotProduct4(body->m_omega).GetScalar();

			maxAccel = dgMax(maxAccel, accel2);
			maxAlpha = dgMax(maxAlpha, alpha2);
			maxSpeed = dgMax(maxSpeed, speed2);
			maxOmega = dgMax(maxOmega, omega2);
			bool equilibrium = (accel2 < accelFreeze) && (alpha2 < accelFreeze) && (speed2 < speedFreeze) && (omega2 < speedFreeze);
			if (equilibrium) {
				dgVector veloc(body->m_veloc * velocDragVect);
				dgVector omega(body->m_omega * velocDragVect);
				body->m_veloc = (veloc.DotProduct4(veloc) > m_velocTol) & veloc;
				body->m_omega = (omega.DotProduct4(omega) > m_velocTol) & omega;
			}

			body->m_equilibrium = equilibrium ? 1 : 0;
			//body->m_equilibrium &= body->m_autoSleep;
			stackSleeping &= equilibrium;
			//isClusterResting &= (body->m_autoSleep & equilibrium);
			if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
				dgDynamicBody* const dynBody = (dgDynamicBody*)body;
				sleepCounter = dgMin(sleepCounter, dynBody->m_sleepingCounter);
				dynBody->m_sleepingCounter++;
			}

			body->UpdateCollisionMatrix(timestep, threadID);
		}
	}

//	if (isClusterResting && cluster->m_jointCount) {
	if (cluster->m_jointCount) {
		if (stackSleeping) {
			for (dgInt32 i = 0; i < count; i++) {
				dgBody* const body = bodyArray[i].m_body;
				dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
				body->m_accel = dgVector::m_zero;
				body->m_alpha = dgVector::m_zero;
				body->m_veloc = dgVector::m_zero;
				body->m_omega = dgVector::m_zero;
				body->m_sleeping = body->m_autoSleep;
			}
		} else {
			bool state = (maxAccel > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAccel) ||
						 (maxAlpha > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAlpha) ||
						 (maxSpeed > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxVeloc) || 
						 (maxOmega > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxOmega);
			if (state) {
				for (dgInt32 i = 0; i < count; i++) {
					dgBody* const body = bodyArray[i].m_body;
					if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
						dgDynamicBody* const dynBody = (dgDynamicBody*)body;
						dynBody->m_sleepingCounter = 0;
					}
				}
			} else {
				dgInt32 timeScaleSleepCount = dgInt32(dgFloat32(60.0f) * sleepCounter * timestep);

				dgInt32 index = DG_SLEEP_ENTRIES;
				for (dgInt32 i = 1; i < DG_SLEEP_ENTRIES; i ++) {
					if (world->m_sleepTable[i].m_steps > timeScaleSleepCount) {
						index = i;
						break;
					}
				}
				index --;

				bool state1 = (maxAccel < world->m_sleepTable[index].m_maxAccel) &&
							  (maxAlpha < world->m_sleepTable[index].m_maxAlpha) &&
					          (maxSpeed < world->m_sleepTable[index].m_maxVeloc) &&
					          (maxOmega < world->m_sleepTable[index].m_maxOmega);
				if (state1) {
					for (dgInt32 i = 0; i < count; i++) {
						dgBody* const body = bodyArray[i].m_body;
						body->m_accel = dgVector::m_zero;
						body->m_alpha = dgVector::m_zero;
						body->m_veloc = dgVector::m_zero;
						body->m_omega = dgVector::m_zero;
						body->m_sleeping = body->m_autoSleep;
						if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
							dgDynamicBody* const dynBody = (dgDynamicBody*)body;
							dynBody->m_sleepingCounter = 0;
						}
					}
				}
			}
		}
	}

/*
if (stackSleeping) {
	for (dgInt32 i = 0; i < count; i++) {
		dgBody* const body = bodyArray[i].m_body;
		dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
		body->m_accel = dgVector::m_zero;
		body->m_alpha = dgVector::m_zero;
		body->m_veloc = dgVector::m_zero;
		body->m_omega = dgVector::m_zero;
		body->m_sleeping = body->m_autoSleep;
	}
}
else {
	const bool state = (maxAccel > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAccel) || (maxAlpha > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxAlpha) ||
		(maxSpeed > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxVeloc) || (maxOmega > world->m_sleepTable[DG_SLEEP_ENTRIES - 1].m_maxOmega);
	if (state) {
		for (dgInt32 i = 0; i < count; i++) {
			dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
			dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI));
			body->m_sleepingCounter = 0;
		}
	}
	else {
		dgInt32 index = 0;
		for (dgInt32 i = 0; i < DG_SLEEP_ENTRIES; i++) {
			if ((maxAccel <= world->m_sleepTable[i].m_maxAccel) &&
				(maxAlpha <= world->m_sleepTable[i].m_maxAlpha) &&
				(maxSpeed <= world->m_sleepTable[i].m_maxVeloc) &&
				(maxOmega <= world->m_sleepTable[i].m_maxOmega)) {
				index = i;
				break;
			}
		}

		dgInt32 timeScaleSleepCount = dgInt32(dgFloat32(60.0f) * sleepCounter * timestep);
		if (timeScaleSleepCount > world->m_sleepTable[index].m_steps) {
			for (dgInt32 i = 0; i < count; i++) {
				dgBody* const body = bodyArray[i].m_body;
				dgAssert(body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || body->IsRTTIType(dgBody::m_kinematicBodyRTTI));
				body->m_accel = dgVector::m_zero;
				body->m_alpha = dgVector::m_zero;
				body->m_veloc = dgVector::m_zero;
				body->m_omega = dgVector::m_zero;
				body->m_equilibrium = 1;
				body->m_sleeping = body->m_autoSleep;
			}
		}
		else {
			sleepCounter++;
			for (dgInt32 i = 0; i < count; i++) {
				dgDynamicBody* const body = (dgDynamicBody*)bodyArray[i].m_body;
				if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
					body->m_sleepingCounter = sleepCounter;
				}
			}
		}
	}
*/
#endif
}


