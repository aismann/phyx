#include "Solver.h"

#include "base/Parallel.h"
#include "base/SIMD.h"

const float kProductiveImpulse = 1e-4f;
const float kFrictionCoefficient = 0.3f;

Solver::Solver()
{
}

NOINLINE float Solver::SolveJointsAoS(WorkQueue& queue, RigidBody* bodies, int bodiesCount, int contactIterationsCount, int penetrationIterationsCount)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsAoS", -1);

    SolvePrepareAoS(bodies, bodiesCount);

    {
        MICROPROFILE_SCOPEI("Physics", "Prepare", -1);

        {
            MICROPROFILE_SCOPEI("Physics", "RefreshJoints", -1);

            parallelFor(queue, contactJoints.data(), contactJoints.size(), 8, [](ContactJoint& j, int) { j.Refresh(); });
        }

        PreStepJointsAoS(0, contactJoints.size());
    }

    {
        MICROPROFILE_SCOPEI("Physics", "Impulse", -1);

        for (int iterationIndex = 0; iterationIndex < contactIterationsCount; iterationIndex++)
        {
            bool productive = SolveJointsImpulsesAoS(0, contactJoints.size(), iterationIndex);

            if (!productive) break;
        }
    }

    {
        MICROPROFILE_SCOPEI("Physics", "Displacement", -1);

        for (int iterationIndex = 0; iterationIndex < penetrationIterationsCount; iterationIndex++)
        {
            bool productive = SolveJointsDisplacementAoS(0, contactJoints.size(), iterationIndex);

            if (!productive) break;
        }
    }

    return SolveFinishAoS();
}

NOINLINE float Solver::SolveJointsSoA_Scalar(WorkQueue& queue, RigidBody* bodies, int bodiesCount, ContactPoint* contactPoints, int contactIterationsCount, int penetrationIterationsCount)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsSoA_Scalar", -1);

    return SolveJointsSoA(queue, joint_packed1, bodies, bodiesCount, contactPoints, contactIterationsCount, penetrationIterationsCount);
}

NOINLINE float Solver::SolveJointsSoA_SSE2(WorkQueue& queue, RigidBody* bodies, int bodiesCount, ContactPoint* contactPoints, int contactIterationsCount, int penetrationIterationsCount)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsSoA_SSE2", -1);

    return SolveJointsSoA(queue, joint_packed4, bodies, bodiesCount, contactPoints, contactIterationsCount, penetrationIterationsCount);
}

#ifdef __AVX2__
NOINLINE float Solver::SolveJointsSoA_AVX2(WorkQueue& queue, RigidBody* bodies, int bodiesCount, ContactPoint* contactPoints, int contactIterationsCount, int penetrationIterationsCount)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsSoA_AVX2", -1);

    return SolveJointsSoA(queue, joint_packed8, bodies, bodiesCount, contactPoints, contactIterationsCount, penetrationIterationsCount);
}
#endif

template <int N>
float Solver::SolveJointsSoA(WorkQueue& queue, AlignedArray<ContactJointPacked<N>>& joint_packed, RigidBody* bodies, int bodiesCount, ContactPoint* contactPoints, int contactIterationsCount, int penetrationIterationsCount)
{
    int groupOffset = SolvePrepareSoA(joint_packed, bodies, bodiesCount, N);

    {
        MICROPROFILE_SCOPEI("Physics", "Prepare", -1);

        RefreshJointsSoA<N>(joint_packed.data, 0, groupOffset, contactPoints);
        RefreshJointsSoA<1>(joint_packed.data, groupOffset, contactJoints.size(), contactPoints);

        PreStepJointsSoA<N>(joint_packed.data, 0, groupOffset);
        PreStepJointsSoA<1>(joint_packed.data, groupOffset, contactJoints.size());
    }

    {
        MICROPROFILE_SCOPEI("Physics", "Impulse", -1);

        for (int iterationIndex = 0; iterationIndex < contactIterationsCount; iterationIndex++)
        {
            bool productive = false;

            productive |= SolveJointsImpulsesSoA<N>(joint_packed.data, 0, groupOffset, iterationIndex);
            productive |= SolveJointsImpulsesSoA<1>(joint_packed.data, groupOffset, contactJoints.size(), iterationIndex);

            if (!productive) break;
        }
    }

    {
        MICROPROFILE_SCOPEI("Physics", "Displacement", -1);

        for (int iterationIndex = 0; iterationIndex < penetrationIterationsCount; iterationIndex++)
        {
            bool productive = false;

            productive |= SolveJointsDisplacementSoA<N>(joint_packed.data, 0, groupOffset, iterationIndex);
            productive |= SolveJointsDisplacementSoA<1>(joint_packed.data, groupOffset, contactJoints.size(), iterationIndex);

            if (!productive) break;
        }
    }

    return SolveFinishSoA(joint_packed, bodies, bodiesCount);
}

NOINLINE int Solver::SolvePrepareIndicesSoA(int bodiesCount, int groupSizeTarget)
{
    MICROPROFILE_SCOPEI("Physics", "SolvePrepareIndicesSoA", -1);

    int jointCount = contactJoints.size();

    if (groupSizeTarget == 1)
    {
        for (int i = 0; i < jointCount; ++i)
            joint_index[i] = i;

        return jointCount;
    }
    else
    {
        jointGroup_bodies.resize(bodiesCount);
        jointGroup_joints.resize(jointCount);

        for (int i = 0; i < bodiesCount; ++i)
            jointGroup_bodies[i] = 0;

        for (int i = 0; i < jointCount; ++i)
            jointGroup_joints[i] = i;

        int tag = 0;

        int groupOffset = 0;

        while (jointGroup_joints.size >= groupSizeTarget)
        {
            // gather a group of N joints with non-overlapping bodies
            int groupSize = 0;

            tag++;

            for (int i = 0; i < jointGroup_joints.size && groupSize < groupSizeTarget;)
            {
                int jointIndex = jointGroup_joints[i];
                ContactJoint& joint = contactJoints[jointIndex];

                if (jointGroup_bodies[joint.body1Index] < tag && jointGroup_bodies[joint.body2Index] < tag)
                {
                    jointGroup_bodies[joint.body1Index] = tag;
                    jointGroup_bodies[joint.body2Index] = tag;

                    joint_index[groupOffset + groupSize] = jointIndex;
                    groupSize++;

                    jointGroup_joints[i] = jointGroup_joints[jointGroup_joints.size - 1];
                    jointGroup_joints.size--;
                }
                else
                {
                    i++;
                }
            }

            groupOffset += groupSize;

            if (groupSize < groupSizeTarget)
                break;
        }

        // fill in the rest of the joints sequentially - they don't form a group so we'll have to solve them 1 by 1
        for (int i = 0; i < jointGroup_joints.size; ++i)
            joint_index[groupOffset + i] = jointGroup_joints[i];

        return (groupOffset / groupSizeTarget) * groupSizeTarget;
    }
}

NOINLINE void Solver::SolvePrepareAoS(RigidBody* bodies, int bodiesCount)
{
    MICROPROFILE_SCOPEI("Physics", "SolvePrepareAoS", -1);

    for (int bodyIndex = 0; bodyIndex < bodiesCount; ++bodyIndex)
    {
        bodies[bodyIndex].lastIteration = -1;
        bodies[bodyIndex].lastDisplacementIteration = -1;
    }
}

NOINLINE float Solver::SolveFinishAoS()
{
    MICROPROFILE_SCOPEI("Physics", "SolveFinishAoS", -1);

    int iterationSum = 0;

    for (size_t jointIndex = 0; jointIndex < contactJoints.size(); jointIndex++)
    {
        ContactJoint& joint = contactJoints[jointIndex];

        iterationSum += std::max(joint.body1->lastIteration, joint.body2->lastIteration) + 2;
        iterationSum += std::max(joint.body1->lastDisplacementIteration, joint.body2->lastDisplacementIteration) + 2;
    }

    return float(iterationSum) / float(contactJoints.size());
}

template <int N>
NOINLINE int Solver::SolvePrepareSoA(
    AlignedArray<ContactJointPacked<N>>& joint_packed,
    RigidBody* bodies, int bodiesCount, int groupSizeTarget)
{
    MICROPROFILE_SCOPEI("Physics", "SolvePrepareSoA", -1);

    {
        MICROPROFILE_SCOPEI("Physics", "CopyBodies", -1);

        solveBodiesParams.resize(bodiesCount);
        solveBodiesImpulse.resize(bodiesCount);
        solveBodiesDisplacement.resize(bodiesCount);

        for (int i = 0; i < bodiesCount; ++i)
        {
            solveBodiesParams[i].invMass = bodies[i].invMass;
            solveBodiesParams[i].invInertia = bodies[i].invInertia;
            solveBodiesParams[i].coords_pos = bodies[i].coords.pos;
            solveBodiesParams[i].coords_xVector = bodies[i].coords.xVector;
            solveBodiesParams[i].coords_yVector = bodies[i].coords.yVector;

            solveBodiesImpulse[i].velocity = bodies[i].velocity;
            solveBodiesImpulse[i].angularVelocity = bodies[i].angularVelocity;
            solveBodiesImpulse[i].lastIteration = -1;

            solveBodiesDisplacement[i].velocity = bodies[i].displacingVelocity;
            solveBodiesDisplacement[i].angularVelocity = bodies[i].displacingAngularVelocity;
            solveBodiesDisplacement[i].lastIteration = -1;
        }
    }

    int jointCount = contactJoints.size();

    joint_index.resize(jointCount);

    joint_packed.resize(jointCount);

    int groupOffset = SolvePrepareIndicesSoA(bodiesCount, groupSizeTarget);

    {
        MICROPROFILE_SCOPEI("Physics", "CopyJoints", -1);

        for (int i = 0; i < jointCount; ++i)
        {
            ContactJoint& joint = contactJoints[joint_index[i]];

            ContactJointPacked<N>& jointP = joint_packed[unsigned(i) / N];
            int iP = i & (N - 1);

            jointP.body1Index[iP] = joint.body1Index;
            jointP.body2Index[iP] = joint.body2Index;
            jointP.contactPointIndex[iP] = joint.collisionIndex;

            jointP.normalLimiter_normalProjector1X[iP] = joint.normalLimiter.normalProjector1.x;
            jointP.normalLimiter_normalProjector1Y[iP] = joint.normalLimiter.normalProjector1.y;
            jointP.normalLimiter_normalProjector2X[iP] = joint.normalLimiter.normalProjector2.x;
            jointP.normalLimiter_normalProjector2Y[iP] = joint.normalLimiter.normalProjector2.y;
            jointP.normalLimiter_angularProjector1[iP] = joint.normalLimiter.angularProjector1;
            jointP.normalLimiter_angularProjector2[iP] = joint.normalLimiter.angularProjector2;

            jointP.normalLimiter_compMass1_linearX[iP] = joint.normalLimiter.compMass1_linear.x;
            jointP.normalLimiter_compMass1_linearY[iP] = joint.normalLimiter.compMass1_linear.y;
            jointP.normalLimiter_compMass2_linearX[iP] = joint.normalLimiter.compMass2_linear.x;
            jointP.normalLimiter_compMass2_linearY[iP] = joint.normalLimiter.compMass2_linear.y;
            jointP.normalLimiter_compMass1_angular[iP] = joint.normalLimiter.compMass1_angular;
            jointP.normalLimiter_compMass2_angular[iP] = joint.normalLimiter.compMass2_angular;
            jointP.normalLimiter_compInvMass[iP] = joint.normalLimiter.compInvMass;
            jointP.normalLimiter_accumulatedImpulse[iP] = joint.normalLimiter.accumulatedImpulse;

            jointP.normalLimiter_dstVelocity[iP] = joint.normalLimiter.dstVelocity;
            jointP.normalLimiter_dstDisplacingVelocity[iP] = joint.normalLimiter.dstDisplacingVelocity;
            jointP.normalLimiter_accumulatedDisplacingImpulse[iP] = joint.normalLimiter.accumulatedDisplacingImpulse;

            jointP.frictionLimiter_normalProjector1X[iP] = joint.frictionLimiter.normalProjector1.x;
            jointP.frictionLimiter_normalProjector1Y[iP] = joint.frictionLimiter.normalProjector1.y;
            jointP.frictionLimiter_normalProjector2X[iP] = joint.frictionLimiter.normalProjector2.x;
            jointP.frictionLimiter_normalProjector2Y[iP] = joint.frictionLimiter.normalProjector2.y;
            jointP.frictionLimiter_angularProjector1[iP] = joint.frictionLimiter.angularProjector1;
            jointP.frictionLimiter_angularProjector2[iP] = joint.frictionLimiter.angularProjector2;

            jointP.frictionLimiter_compMass1_linearX[iP] = joint.frictionLimiter.compMass1_linear.x;
            jointP.frictionLimiter_compMass1_linearY[iP] = joint.frictionLimiter.compMass1_linear.y;
            jointP.frictionLimiter_compMass2_linearX[iP] = joint.frictionLimiter.compMass2_linear.x;
            jointP.frictionLimiter_compMass2_linearY[iP] = joint.frictionLimiter.compMass2_linear.y;
            jointP.frictionLimiter_compMass1_angular[iP] = joint.frictionLimiter.compMass1_angular;
            jointP.frictionLimiter_compMass2_angular[iP] = joint.frictionLimiter.compMass2_angular;
            jointP.frictionLimiter_compInvMass[iP] = joint.frictionLimiter.compInvMass;
            jointP.frictionLimiter_accumulatedImpulse[iP] = joint.frictionLimiter.accumulatedImpulse;
        }
    }

    return groupOffset;
}

template <int N>
NOINLINE float Solver::SolveFinishSoA(
    AlignedArray<ContactJointPacked<N>>& joint_packed,
    RigidBody* bodies, int bodiesCount)
{
    MICROPROFILE_SCOPEI("Physics", "SolveFinishSoA", -1);

    for (int i = 0; i < bodiesCount; ++i)
    {
        bodies[i].velocity = solveBodiesImpulse[i].velocity;
        bodies[i].angularVelocity = solveBodiesImpulse[i].angularVelocity;

        bodies[i].displacingVelocity = solveBodiesDisplacement[i].velocity;
        bodies[i].displacingAngularVelocity = solveBodiesDisplacement[i].angularVelocity;
    }

    int jointCount = contactJoints.size();

    for (int i = 0; i < jointCount; ++i)
    {
        ContactJoint& joint = contactJoints[joint_index[i]];

        ContactJointPacked<N>& jointP = joint_packed[unsigned(i) / N];
        int iP = i & (N - 1);

        joint.normalLimiter.accumulatedImpulse = jointP.normalLimiter_accumulatedImpulse[iP];
        joint.normalLimiter.accumulatedDisplacingImpulse = jointP.normalLimiter_accumulatedDisplacingImpulse[iP];
        joint.frictionLimiter.accumulatedImpulse = jointP.frictionLimiter_accumulatedImpulse[iP];
    }

    int iterationSum = 0;

    for (int i = 0; i < jointCount; ++i)
    {
        ContactJointPacked<N>& jointP = joint_packed[unsigned(i) / N];
        int iP = i & (N - 1);

        unsigned int bi1 = jointP.body1Index[iP];
        unsigned int bi2 = jointP.body2Index[iP];

        iterationSum += std::max(solveBodiesImpulse[bi1].lastIteration, solveBodiesImpulse[bi2].lastIteration) + 2;
        iterationSum += std::max(solveBodiesDisplacement[bi1].lastIteration, solveBodiesDisplacement[bi2].lastIteration) + 2;
    }

    return float(iterationSum) / float(jointCount);
}

NOINLINE void Solver::PreStepJointsAoS(int jointBegin, int jointEnd)
{
    MICROPROFILE_SCOPEI("Physics", "PreStepJointsAoS", -1);

    for (int jointIndex = jointBegin; jointIndex < jointEnd; jointIndex++)
    {
        ContactJoint& joint = contactJoints[jointIndex];

        RigidBody* body1 = joint.body1;
        RigidBody* body2 = joint.body2;

        body1->velocity.x += joint.normalLimiter.compMass1_linear.x * joint.normalLimiter.accumulatedImpulse;
        body1->velocity.y += joint.normalLimiter.compMass1_linear.y * joint.normalLimiter.accumulatedImpulse;
        body1->angularVelocity += joint.normalLimiter.compMass1_angular * joint.normalLimiter.accumulatedImpulse;
        body2->velocity.x += joint.normalLimiter.compMass2_linear.x * joint.normalLimiter.accumulatedImpulse;
        body2->velocity.y += joint.normalLimiter.compMass2_linear.y * joint.normalLimiter.accumulatedImpulse;
        body2->angularVelocity += joint.normalLimiter.compMass2_angular * joint.normalLimiter.accumulatedImpulse;

        body1->velocity.x += joint.frictionLimiter.compMass1_linear.x * joint.frictionLimiter.accumulatedImpulse;
        body1->velocity.y += joint.frictionLimiter.compMass1_linear.y * joint.frictionLimiter.accumulatedImpulse;
        body1->angularVelocity += joint.frictionLimiter.compMass1_angular * joint.frictionLimiter.accumulatedImpulse;
        body2->velocity.x += joint.frictionLimiter.compMass2_linear.x * joint.frictionLimiter.accumulatedImpulse;
        body2->velocity.y += joint.frictionLimiter.compMass2_linear.y * joint.frictionLimiter.accumulatedImpulse;
        body2->angularVelocity += joint.frictionLimiter.compMass2_angular * joint.frictionLimiter.accumulatedImpulse;
    }
}

NOINLINE bool Solver::SolveJointsImpulsesAoS(int jointBegin, int jointEnd, int iterationIndex)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsImpulsesAoS", -1);

    bool productive = false;

    for (int jointIndex = jointBegin; jointIndex < jointEnd; jointIndex++)
    {
        ContactJoint& joint = contactJoints[jointIndex];

        RigidBody* body1 = joint.body1;
        RigidBody* body2 = joint.body2;

        if (body1->lastIteration < iterationIndex - 1 && body2->lastIteration < iterationIndex - 1)
            continue;

        float normaldV = joint.normalLimiter.dstVelocity;

        normaldV -= joint.normalLimiter.normalProjector1.x * body1->velocity.x;
        normaldV -= joint.normalLimiter.normalProjector1.y * body1->velocity.y;
        normaldV -= joint.normalLimiter.angularProjector1 * body1->angularVelocity;

        normaldV -= joint.normalLimiter.normalProjector2.x * body2->velocity.x;
        normaldV -= joint.normalLimiter.normalProjector2.y * body2->velocity.y;
        normaldV -= joint.normalLimiter.angularProjector2 * body2->angularVelocity;

        float normalDeltaImpulse = normaldV * joint.normalLimiter.compInvMass;

        if (normalDeltaImpulse + joint.normalLimiter.accumulatedImpulse < 0.0f)
            normalDeltaImpulse = -joint.normalLimiter.accumulatedImpulse;

        body1->velocity.x += joint.normalLimiter.compMass1_linear.x * normalDeltaImpulse;
        body1->velocity.y += joint.normalLimiter.compMass1_linear.y * normalDeltaImpulse;
        body1->angularVelocity += joint.normalLimiter.compMass1_angular * normalDeltaImpulse;
        body2->velocity.x += joint.normalLimiter.compMass2_linear.x * normalDeltaImpulse;
        body2->velocity.y += joint.normalLimiter.compMass2_linear.y * normalDeltaImpulse;
        body2->angularVelocity += joint.normalLimiter.compMass2_angular * normalDeltaImpulse;

        joint.normalLimiter.accumulatedImpulse += normalDeltaImpulse;

        float frictiondV = 0;

        frictiondV -= joint.frictionLimiter.normalProjector1.x * body1->velocity.x;
        frictiondV -= joint.frictionLimiter.normalProjector1.y * body1->velocity.y;
        frictiondV -= joint.frictionLimiter.angularProjector1 * body1->angularVelocity;

        frictiondV -= joint.frictionLimiter.normalProjector2.x * body2->velocity.x;
        frictiondV -= joint.frictionLimiter.normalProjector2.y * body2->velocity.y;
        frictiondV -= joint.frictionLimiter.angularProjector2 * body2->angularVelocity;

        float frictionDeltaImpulse = frictiondV * joint.frictionLimiter.compInvMass;

        float reactionForce = joint.normalLimiter.accumulatedImpulse;
        float accumulatedImpulse = joint.frictionLimiter.accumulatedImpulse;

        float frictionForce = accumulatedImpulse + frictionDeltaImpulse;
        float frictionCoefficient = kFrictionCoefficient;

        if (fabsf(frictionForce) > (reactionForce * frictionCoefficient))
        {
            float dir = frictionForce > 0.0f ? 1.0f : -1.0f;
            frictionForce = dir * reactionForce * frictionCoefficient;
            frictionDeltaImpulse = frictionForce - accumulatedImpulse;
        }

        joint.frictionLimiter.accumulatedImpulse += frictionDeltaImpulse;

        body1->velocity.x += joint.frictionLimiter.compMass1_linear.x * frictionDeltaImpulse;
        body1->velocity.y += joint.frictionLimiter.compMass1_linear.y * frictionDeltaImpulse;
        body1->angularVelocity += joint.frictionLimiter.compMass1_angular * frictionDeltaImpulse;

        body2->velocity.x += joint.frictionLimiter.compMass2_linear.x * frictionDeltaImpulse;
        body2->velocity.y += joint.frictionLimiter.compMass2_linear.y * frictionDeltaImpulse;
        body2->angularVelocity += joint.frictionLimiter.compMass2_angular * frictionDeltaImpulse;

        float cumulativeImpulse = std::max(fabsf(normalDeltaImpulse), fabsf(frictionDeltaImpulse));

        if (cumulativeImpulse > kProductiveImpulse)
        {
            body1->lastIteration = iterationIndex;
            body2->lastIteration = iterationIndex;
            productive = true;
        }
    }

    return productive;
}

NOINLINE bool Solver::SolveJointsDisplacementAoS(int jointBegin, int jointEnd, int iterationIndex)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsDisplacementAoS", -1);

    bool productive = false;

    for (int jointIndex = jointBegin; jointIndex < jointEnd; jointIndex++)
    {
        ContactJoint& joint = contactJoints[jointIndex];

        RigidBody* body1 = joint.body1;
        RigidBody* body2 = joint.body2;

        if (body1->lastDisplacementIteration < iterationIndex - 1 && body2->lastDisplacementIteration < iterationIndex - 1)
            continue;

        float dV = joint.normalLimiter.dstDisplacingVelocity;

        dV -= joint.normalLimiter.normalProjector1.x * body1->displacingVelocity.x;
        dV -= joint.normalLimiter.normalProjector1.y * body1->displacingVelocity.y;
        dV -= joint.normalLimiter.angularProjector1 * body1->displacingAngularVelocity;

        dV -= joint.normalLimiter.normalProjector2.x * body2->displacingVelocity.x;
        dV -= joint.normalLimiter.normalProjector2.y * body2->displacingVelocity.y;
        dV -= joint.normalLimiter.angularProjector2 * body2->displacingAngularVelocity;

        float displacingDeltaImpulse = dV * joint.normalLimiter.compInvMass;

        if (displacingDeltaImpulse + joint.normalLimiter.accumulatedDisplacingImpulse < 0.0f)
            displacingDeltaImpulse = -joint.normalLimiter.accumulatedDisplacingImpulse;

        body1->displacingVelocity.x += joint.normalLimiter.compMass1_linear.x * displacingDeltaImpulse;
        body1->displacingVelocity.y += joint.normalLimiter.compMass1_linear.y * displacingDeltaImpulse;
        body1->displacingAngularVelocity += joint.normalLimiter.compMass1_angular * displacingDeltaImpulse;

        body2->displacingVelocity.x += joint.normalLimiter.compMass2_linear.x * displacingDeltaImpulse;
        body2->displacingVelocity.y += joint.normalLimiter.compMass2_linear.y * displacingDeltaImpulse;
        body2->displacingAngularVelocity += joint.normalLimiter.compMass2_angular * displacingDeltaImpulse;

        joint.normalLimiter.accumulatedDisplacingImpulse += displacingDeltaImpulse;

        if (fabsf(displacingDeltaImpulse) > kProductiveImpulse)
        {
            body1->lastDisplacementIteration = iterationIndex;
            body2->lastDisplacementIteration = iterationIndex;
            productive = true;
        }
    }

    return productive;
}

template <typename Vf>
static void RefreshLimiter(
    Vf& normalProjector1X, Vf& normalProjector1Y,
    Vf& normalProjector2X, Vf& normalProjector2Y,
    Vf& angularProjector1, Vf& angularProjector2,
    Vf& compMass1_linearX, Vf& compMass1_linearY,
    Vf& compMass2_linearX, Vf& compMass2_linearY,
    Vf& compMass1_angular, Vf& compMass2_angular,
    Vf& compInvMass,
    const Vf& n1X, const Vf& n1Y, const Vf& n2X, const Vf& n2Y, const Vf& w1X, const Vf& w1Y, const Vf& w2X, const Vf& w2Y,
    const Vf& body1_invMass, const Vf& body1_invInertia, const Vf& body2_invMass, const Vf& body2_invInertia)
{
    normalProjector1X = n1X;
    normalProjector1Y = n1Y;
    normalProjector2X = n2X;
    normalProjector2Y = n2Y;
    angularProjector1 = n1X * w1Y - n1Y * w1X;
    angularProjector2 = n2X * w2Y - n2Y * w2X;

    compMass1_linearX = normalProjector1X * body1_invMass;
    compMass1_linearY = normalProjector1Y * body1_invMass;
    compMass1_angular = angularProjector1 * body1_invInertia;
    compMass2_linearX = normalProjector2X * body2_invMass;
    compMass2_linearY = normalProjector2Y * body2_invMass;
    compMass2_angular = angularProjector2 * body2_invInertia;

    Vf compMass1 = normalProjector1X * compMass1_linearX + normalProjector1Y * compMass1_linearY + angularProjector1 * compMass1_angular;
    Vf compMass2 = normalProjector2X * compMass2_linearX + normalProjector2Y * compMass2_linearY + angularProjector2 * compMass2_angular;

    Vf compMass = compMass1 + compMass2;

    compInvMass = select(Vf::zero(), Vf::one(1) / compMass, abs(compMass) > Vf::zero());
}

template <int VN, int N>
NOINLINE void Solver::RefreshJointsSoA(ContactJointPacked<N>* joint_packed, int jointBegin, int jointEnd, ContactPoint* contactPoints)
{
    MICROPROFILE_SCOPEI("Physics", "RefreshJointsSoA", -1);

    typedef simd::VNf<VN> Vf;
    typedef simd::VNi<VN> Vi;
    typedef simd::VNb<VN> Vb;

    assert(jointBegin % VN == 0 && jointEnd % VN == 0);

    for (int jointIndex = jointBegin; jointIndex < jointEnd; jointIndex += VN)
    {
        int i = jointIndex;

        ContactJointPacked<N>& jointP = joint_packed[unsigned(i) / N];
        int iP = (VN == N) ? 0 : i & (N - 1);

        Vf body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf;
        Vf body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf;

        Vf body1_invMass, body1_invInertia, body1_coords_posX, body1_coords_posY;
        Vf body1_coords_xVectorX, body1_coords_xVectorY, body1_coords_yVectorX, body1_coords_yVectorY;

        Vf body2_invMass, body2_invInertia, body2_coords_posX, body2_coords_posY;
        Vf body2_coords_xVectorX, body2_coords_xVectorY, body2_coords_yVectorX, body2_coords_yVectorY;

        Vf collision_delta1X, collision_delta1Y, collision_delta2X, collision_delta2Y;
        Vf collision_normalX, collision_normalY;
        Vf dummy;

        loadindexed4(
            body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf,
            solveBodiesImpulse.data, jointP.body1Index + iP, sizeof(SolveBody));

        loadindexed4(
            body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf,
            solveBodiesImpulse.data, jointP.body2Index + iP, sizeof(SolveBody));

        loadindexed8(
            body1_invMass, body1_invInertia, body1_coords_posX, body1_coords_posY,
            body1_coords_xVectorX, body1_coords_xVectorY, body1_coords_yVectorX, body1_coords_yVectorY,
            solveBodiesParams.data, jointP.body1Index + iP, sizeof(SolveBodyParams));

        loadindexed8(
            body2_invMass, body2_invInertia, body2_coords_posX, body2_coords_posY,
            body2_coords_xVectorX, body2_coords_xVectorY, body2_coords_yVectorX, body2_coords_yVectorY,
            solveBodiesParams.data, jointP.body2Index + iP, sizeof(SolveBodyParams));

        loadindexed8(
            collision_delta1X, collision_delta1Y, collision_delta2X, collision_delta2Y,
            collision_normalX, collision_normalY, dummy, dummy,
            contactPoints, jointP.contactPointIndex + iP, sizeof(ContactPoint));

        Vf j_normalLimiter_normalProjector1X, j_normalLimiter_normalProjector1Y;
        Vf j_normalLimiter_normalProjector2X, j_normalLimiter_normalProjector2Y;
        Vf j_normalLimiter_angularProjector1, j_normalLimiter_angularProjector2;
        Vf j_normalLimiter_compMass1_linearX, j_normalLimiter_compMass1_linearY;
        Vf j_normalLimiter_compMass2_linearX, j_normalLimiter_compMass2_linearY;
        Vf j_normalLimiter_compMass1_angular, j_normalLimiter_compMass2_angular;
        Vf j_normalLimiter_compInvMass;
        Vf j_normalLimiter_accumulatedImpulse;
        Vf j_normalLimiter_dstVelocity;
        Vf j_normalLimiter_dstDisplacingVelocity;
        Vf j_normalLimiter_accumulatedDisplacingImpulse;

        Vf j_frictionLimiter_normalProjector1X, j_frictionLimiter_normalProjector1Y;
        Vf j_frictionLimiter_normalProjector2X, j_frictionLimiter_normalProjector2Y;
        Vf j_frictionLimiter_angularProjector1, j_frictionLimiter_angularProjector2;
        Vf j_frictionLimiter_compMass1_linearX, j_frictionLimiter_compMass1_linearY;
        Vf j_frictionLimiter_compMass2_linearX, j_frictionLimiter_compMass2_linearY;
        Vf j_frictionLimiter_compMass1_angular, j_frictionLimiter_compMass2_angular;
        Vf j_frictionLimiter_compInvMass;

        Vf point1X = collision_delta1X + body1_coords_posX;
        Vf point1Y = collision_delta1Y + body1_coords_posY;
        Vf point2X = collision_delta2X + body2_coords_posX;
        Vf point2Y = collision_delta2Y + body2_coords_posY;

        Vf w1X = collision_delta1X;
        Vf w1Y = collision_delta1Y;
        Vf w2X = point1X - body2_coords_posX;
        Vf w2Y = point1Y - body2_coords_posY;

        // Normal limiter
        RefreshLimiter(
            j_normalLimiter_normalProjector1X, j_normalLimiter_normalProjector1Y,
            j_normalLimiter_normalProjector2X, j_normalLimiter_normalProjector2Y,
            j_normalLimiter_angularProjector1, j_normalLimiter_angularProjector2,
            j_normalLimiter_compMass1_linearX, j_normalLimiter_compMass1_linearY,
            j_normalLimiter_compMass2_linearX, j_normalLimiter_compMass2_linearY,
            j_normalLimiter_compMass1_angular, j_normalLimiter_compMass2_angular,
            j_normalLimiter_compInvMass,
            collision_normalX, collision_normalY, -collision_normalX, -collision_normalY,
            w1X, w1Y, w2X, w2Y,
            body1_invMass, body1_invInertia, body2_invMass, body2_invInertia);

        Vf bounce = Vf::zero();
        Vf deltaVelocity = Vf::one(1.f);
        Vf maxPenetrationVelocity = Vf::one(0.1f);
        Vf deltaDepth = Vf::one(1.f);
        Vf errorReduction = Vf::one(0.1f);

        j_normalLimiter_dstDisplacingVelocity = Vf::zero();

        Vf pointVelocity_body1X = (body1_coords_posY - point1Y) * body1_angularVelocity + body1_velocityX;
        Vf pointVelocity_body1Y = (point1X - body1_coords_posX) * body1_angularVelocity + body1_velocityY;

        Vf pointVelocity_body2X = (body2_coords_posY - point2Y) * body2_angularVelocity + body2_velocityX;
        Vf pointVelocity_body2Y = (point2X - body2_coords_posX) * body2_angularVelocity + body2_velocityY;

        Vf relativeVelocityX = pointVelocity_body1X - pointVelocity_body2X;
        Vf relativeVelocityY = pointVelocity_body1Y - pointVelocity_body2Y;

        Vf dv = -bounce * (relativeVelocityX * collision_normalX + relativeVelocityY * collision_normalY);
        Vf depth = (point2X - point1X) * collision_normalX + (point2Y - point1Y) * collision_normalY;

        Vf dstVelocity = max(dv - deltaVelocity, Vf::zero());

        j_normalLimiter_dstVelocity = select(dstVelocity, dstVelocity - maxPenetrationVelocity, depth < deltaDepth);

        j_normalLimiter_dstDisplacingVelocity = errorReduction * max(Vf::zero(), depth - Vf::one(2.0f) * deltaDepth);

        j_normalLimiter_accumulatedDisplacingImpulse = Vf::zero();

        // Friction limiter
        Vf tangentX = -collision_normalY;
        Vf tangentY = collision_normalX;

        RefreshLimiter(
            j_frictionLimiter_normalProjector1X, j_frictionLimiter_normalProjector1Y,
            j_frictionLimiter_normalProjector2X, j_frictionLimiter_normalProjector2Y,
            j_frictionLimiter_angularProjector1, j_frictionLimiter_angularProjector2,
            j_frictionLimiter_compMass1_linearX, j_frictionLimiter_compMass1_linearY,
            j_frictionLimiter_compMass2_linearX, j_frictionLimiter_compMass2_linearY,
            j_frictionLimiter_compMass1_angular, j_frictionLimiter_compMass2_angular,
            j_frictionLimiter_compInvMass,
            tangentX, tangentY, -tangentX, -tangentY,
            w1X, w1Y, w2X, w2Y,
            body1_invMass, body1_invInertia, body2_invMass, body2_invInertia);

        store(j_normalLimiter_normalProjector1X, &jointP.normalLimiter_normalProjector1X[iP]);
        store(j_normalLimiter_normalProjector1Y, &jointP.normalLimiter_normalProjector1Y[iP]);
        store(j_normalLimiter_normalProjector2X, &jointP.normalLimiter_normalProjector2X[iP]);
        store(j_normalLimiter_normalProjector2Y, &jointP.normalLimiter_normalProjector2Y[iP]);
        store(j_normalLimiter_angularProjector1, &jointP.normalLimiter_angularProjector1[iP]);
        store(j_normalLimiter_angularProjector2, &jointP.normalLimiter_angularProjector2[iP]);

        store(j_normalLimiter_compMass1_linearX, &jointP.normalLimiter_compMass1_linearX[iP]);
        store(j_normalLimiter_compMass1_linearY, &jointP.normalLimiter_compMass1_linearY[iP]);
        store(j_normalLimiter_compMass2_linearX, &jointP.normalLimiter_compMass2_linearX[iP]);
        store(j_normalLimiter_compMass2_linearY, &jointP.normalLimiter_compMass2_linearY[iP]);
        store(j_normalLimiter_compMass1_angular, &jointP.normalLimiter_compMass1_angular[iP]);
        store(j_normalLimiter_compMass2_angular, &jointP.normalLimiter_compMass2_angular[iP]);
        store(j_normalLimiter_compInvMass, &jointP.normalLimiter_compInvMass[iP]);
        store(j_normalLimiter_dstVelocity, &jointP.normalLimiter_dstVelocity[iP]);
        store(j_normalLimiter_dstDisplacingVelocity, &jointP.normalLimiter_dstDisplacingVelocity[iP]);
        store(j_normalLimiter_accumulatedDisplacingImpulse, &jointP.normalLimiter_accumulatedDisplacingImpulse[iP]);

        store(j_frictionLimiter_normalProjector1X, &jointP.frictionLimiter_normalProjector1X[iP]);
        store(j_frictionLimiter_normalProjector1Y, &jointP.frictionLimiter_normalProjector1Y[iP]);
        store(j_frictionLimiter_normalProjector2X, &jointP.frictionLimiter_normalProjector2X[iP]);
        store(j_frictionLimiter_normalProjector2Y, &jointP.frictionLimiter_normalProjector2Y[iP]);
        store(j_frictionLimiter_angularProjector1, &jointP.frictionLimiter_angularProjector1[iP]);
        store(j_frictionLimiter_angularProjector2, &jointP.frictionLimiter_angularProjector2[iP]);

        store(j_frictionLimiter_compMass1_linearX, &jointP.frictionLimiter_compMass1_linearX[iP]);
        store(j_frictionLimiter_compMass1_linearY, &jointP.frictionLimiter_compMass1_linearY[iP]);
        store(j_frictionLimiter_compMass2_linearX, &jointP.frictionLimiter_compMass2_linearX[iP]);
        store(j_frictionLimiter_compMass2_linearY, &jointP.frictionLimiter_compMass2_linearY[iP]);
        store(j_frictionLimiter_compMass1_angular, &jointP.frictionLimiter_compMass1_angular[iP]);
        store(j_frictionLimiter_compMass2_angular, &jointP.frictionLimiter_compMass2_angular[iP]);
        store(j_frictionLimiter_compInvMass, &jointP.frictionLimiter_compInvMass[iP]);
    }
}

template <int VN, int N>
NOINLINE void Solver::PreStepJointsSoA(ContactJointPacked<N>* joint_packed, int jointBegin, int jointEnd)
{
    MICROPROFILE_SCOPEI("Physics", "PreStepJointsSoA", -1);

    typedef simd::VNf<VN> Vf;
    typedef simd::VNi<VN> Vi;
    typedef simd::VNb<VN> Vb;

    assert(jointBegin % VN == 0 && jointEnd % VN == 0);

    for (int jointIndex = jointBegin; jointIndex < jointEnd; jointIndex += VN)
    {
        int i = jointIndex;

        ContactJointPacked<N>& jointP = joint_packed[unsigned(i) / N];
        int iP = (VN == N) ? 0 : i & (N - 1);

        Vf body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf;
        Vf body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf;

        loadindexed4(body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf,
            solveBodiesImpulse.data, jointP.body1Index + iP, sizeof(SolveBody));

        loadindexed4(body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf,
            solveBodiesImpulse.data, jointP.body2Index + iP, sizeof(SolveBody));

        Vf j_normalLimiter_compMass1_linearX = Vf::load(&jointP.normalLimiter_compMass1_linearX[iP]);
        Vf j_normalLimiter_compMass1_linearY = Vf::load(&jointP.normalLimiter_compMass1_linearY[iP]);
        Vf j_normalLimiter_compMass2_linearX = Vf::load(&jointP.normalLimiter_compMass2_linearX[iP]);
        Vf j_normalLimiter_compMass2_linearY = Vf::load(&jointP.normalLimiter_compMass2_linearY[iP]);
        Vf j_normalLimiter_compMass1_angular = Vf::load(&jointP.normalLimiter_compMass1_angular[iP]);
        Vf j_normalLimiter_compMass2_angular = Vf::load(&jointP.normalLimiter_compMass2_angular[iP]);
        Vf j_normalLimiter_accumulatedImpulse = Vf::load(&jointP.normalLimiter_accumulatedImpulse[iP]);

        Vf j_frictionLimiter_compMass1_linearX = Vf::load(&jointP.frictionLimiter_compMass1_linearX[iP]);
        Vf j_frictionLimiter_compMass1_linearY = Vf::load(&jointP.frictionLimiter_compMass1_linearY[iP]);
        Vf j_frictionLimiter_compMass2_linearX = Vf::load(&jointP.frictionLimiter_compMass2_linearX[iP]);
        Vf j_frictionLimiter_compMass2_linearY = Vf::load(&jointP.frictionLimiter_compMass2_linearY[iP]);
        Vf j_frictionLimiter_compMass1_angular = Vf::load(&jointP.frictionLimiter_compMass1_angular[iP]);
        Vf j_frictionLimiter_compMass2_angular = Vf::load(&jointP.frictionLimiter_compMass2_angular[iP]);
        Vf j_frictionLimiter_accumulatedImpulse = Vf::load(&jointP.frictionLimiter_accumulatedImpulse[iP]);

        body1_velocityX += j_normalLimiter_compMass1_linearX * j_normalLimiter_accumulatedImpulse;
        body1_velocityY += j_normalLimiter_compMass1_linearY * j_normalLimiter_accumulatedImpulse;
        body1_angularVelocity += j_normalLimiter_compMass1_angular * j_normalLimiter_accumulatedImpulse;

        body2_velocityX += j_normalLimiter_compMass2_linearX * j_normalLimiter_accumulatedImpulse;
        body2_velocityY += j_normalLimiter_compMass2_linearY * j_normalLimiter_accumulatedImpulse;
        body2_angularVelocity += j_normalLimiter_compMass2_angular * j_normalLimiter_accumulatedImpulse;

        body1_velocityX += j_frictionLimiter_compMass1_linearX * j_frictionLimiter_accumulatedImpulse;
        body1_velocityY += j_frictionLimiter_compMass1_linearY * j_frictionLimiter_accumulatedImpulse;
        body1_angularVelocity += j_frictionLimiter_compMass1_angular * j_frictionLimiter_accumulatedImpulse;

        body2_velocityX += j_frictionLimiter_compMass2_linearX * j_frictionLimiter_accumulatedImpulse;
        body2_velocityY += j_frictionLimiter_compMass2_linearY * j_frictionLimiter_accumulatedImpulse;
        body2_angularVelocity += j_frictionLimiter_compMass2_angular * j_frictionLimiter_accumulatedImpulse;

        storeindexed4(body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf,
            solveBodiesImpulse.data, jointP.body1Index + iP, sizeof(SolveBody));

        storeindexed4(body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf,
            solveBodiesImpulse.data, jointP.body2Index + iP, sizeof(SolveBody));
    }
}

template <int VN, int N>
NOINLINE bool Solver::SolveJointsImpulsesSoA(ContactJointPacked<N>* joint_packed, int jointBegin, int jointEnd, int iterationIndex)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsImpulsesSoA", -1);

    typedef simd::VNf<VN> Vf;
    typedef simd::VNi<VN> Vi;
    typedef simd::VNb<VN> Vb;

    assert(jointBegin % VN == 0 && jointEnd % VN == 0);

    Vi iterationIndex0 = Vi::one(iterationIndex);
    Vi iterationIndex2 = Vi::one(iterationIndex - 2);

    Vb productive_any = Vb::zero();

    for (int jointIndex = jointBegin; jointIndex < jointEnd; jointIndex += VN)
    {
        int i = jointIndex;

        ContactJointPacked<N>& jointP = joint_packed[unsigned(i) / N];
        int iP = (VN == N) ? 0 : i & (N - 1);

        Vf body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf;
        Vf body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf;

        loadindexed4(body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf,
            solveBodiesImpulse.data, jointP.body1Index + iP, sizeof(SolveBody));

        loadindexed4(body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf,
            solveBodiesImpulse.data, jointP.body2Index + iP, sizeof(SolveBody));

        Vi body1_lastIteration = bitcast(body1_lastIterationf);
        Vi body2_lastIteration = bitcast(body2_lastIterationf);

        Vb body1_productive = body1_lastIteration > iterationIndex2;
        Vb body2_productive = body2_lastIteration > iterationIndex2;
        Vb body_productive = body1_productive | body2_productive;

        if (none(body_productive))
            continue;

        Vf j_normalLimiter_normalProjector1X = Vf::load(&jointP.normalLimiter_normalProjector1X[iP]);
        Vf j_normalLimiter_normalProjector1Y = Vf::load(&jointP.normalLimiter_normalProjector1Y[iP]);
        Vf j_normalLimiter_normalProjector2X = Vf::load(&jointP.normalLimiter_normalProjector2X[iP]);
        Vf j_normalLimiter_normalProjector2Y = Vf::load(&jointP.normalLimiter_normalProjector2Y[iP]);
        Vf j_normalLimiter_angularProjector1 = Vf::load(&jointP.normalLimiter_angularProjector1[iP]);
        Vf j_normalLimiter_angularProjector2 = Vf::load(&jointP.normalLimiter_angularProjector2[iP]);

        Vf j_normalLimiter_compMass1_linearX = Vf::load(&jointP.normalLimiter_compMass1_linearX[iP]);
        Vf j_normalLimiter_compMass1_linearY = Vf::load(&jointP.normalLimiter_compMass1_linearY[iP]);
        Vf j_normalLimiter_compMass2_linearX = Vf::load(&jointP.normalLimiter_compMass2_linearX[iP]);
        Vf j_normalLimiter_compMass2_linearY = Vf::load(&jointP.normalLimiter_compMass2_linearY[iP]);
        Vf j_normalLimiter_compMass1_angular = Vf::load(&jointP.normalLimiter_compMass1_angular[iP]);
        Vf j_normalLimiter_compMass2_angular = Vf::load(&jointP.normalLimiter_compMass2_angular[iP]);
        Vf j_normalLimiter_compInvMass = Vf::load(&jointP.normalLimiter_compInvMass[iP]);
        Vf j_normalLimiter_accumulatedImpulse = Vf::load(&jointP.normalLimiter_accumulatedImpulse[iP]);
        Vf j_normalLimiter_dstVelocity = Vf::load(&jointP.normalLimiter_dstVelocity[iP]);

        Vf j_frictionLimiter_normalProjector1X = Vf::load(&jointP.frictionLimiter_normalProjector1X[iP]);
        Vf j_frictionLimiter_normalProjector1Y = Vf::load(&jointP.frictionLimiter_normalProjector1Y[iP]);
        Vf j_frictionLimiter_normalProjector2X = Vf::load(&jointP.frictionLimiter_normalProjector2X[iP]);
        Vf j_frictionLimiter_normalProjector2Y = Vf::load(&jointP.frictionLimiter_normalProjector2Y[iP]);
        Vf j_frictionLimiter_angularProjector1 = Vf::load(&jointP.frictionLimiter_angularProjector1[iP]);
        Vf j_frictionLimiter_angularProjector2 = Vf::load(&jointP.frictionLimiter_angularProjector2[iP]);

        Vf j_frictionLimiter_compMass1_linearX = Vf::load(&jointP.frictionLimiter_compMass1_linearX[iP]);
        Vf j_frictionLimiter_compMass1_linearY = Vf::load(&jointP.frictionLimiter_compMass1_linearY[iP]);
        Vf j_frictionLimiter_compMass2_linearX = Vf::load(&jointP.frictionLimiter_compMass2_linearX[iP]);
        Vf j_frictionLimiter_compMass2_linearY = Vf::load(&jointP.frictionLimiter_compMass2_linearY[iP]);
        Vf j_frictionLimiter_compMass1_angular = Vf::load(&jointP.frictionLimiter_compMass1_angular[iP]);
        Vf j_frictionLimiter_compMass2_angular = Vf::load(&jointP.frictionLimiter_compMass2_angular[iP]);
        Vf j_frictionLimiter_compInvMass = Vf::load(&jointP.frictionLimiter_compInvMass[iP]);
        Vf j_frictionLimiter_accumulatedImpulse = Vf::load(&jointP.frictionLimiter_accumulatedImpulse[iP]);

        Vf normaldV = j_normalLimiter_dstVelocity;

        normaldV -= j_normalLimiter_normalProjector1X * body1_velocityX;
        normaldV -= j_normalLimiter_normalProjector1Y * body1_velocityY;
        normaldV -= j_normalLimiter_angularProjector1 * body1_angularVelocity;

        normaldV -= j_normalLimiter_normalProjector2X * body2_velocityX;
        normaldV -= j_normalLimiter_normalProjector2Y * body2_velocityY;
        normaldV -= j_normalLimiter_angularProjector2 * body2_angularVelocity;

        Vf normalDeltaImpulse = normaldV * j_normalLimiter_compInvMass;

        normalDeltaImpulse = max(normalDeltaImpulse, -j_normalLimiter_accumulatedImpulse);

        body1_velocityX += j_normalLimiter_compMass1_linearX * normalDeltaImpulse;
        body1_velocityY += j_normalLimiter_compMass1_linearY * normalDeltaImpulse;
        body1_angularVelocity += j_normalLimiter_compMass1_angular * normalDeltaImpulse;

        body2_velocityX += j_normalLimiter_compMass2_linearX * normalDeltaImpulse;
        body2_velocityY += j_normalLimiter_compMass2_linearY * normalDeltaImpulse;
        body2_angularVelocity += j_normalLimiter_compMass2_angular * normalDeltaImpulse;

        j_normalLimiter_accumulatedImpulse += normalDeltaImpulse;

        Vf frictiondV = Vf::zero();

        frictiondV -= j_frictionLimiter_normalProjector1X * body1_velocityX;
        frictiondV -= j_frictionLimiter_normalProjector1Y * body1_velocityY;
        frictiondV -= j_frictionLimiter_angularProjector1 * body1_angularVelocity;

        frictiondV -= j_frictionLimiter_normalProjector2X * body2_velocityX;
        frictiondV -= j_frictionLimiter_normalProjector2Y * body2_velocityY;
        frictiondV -= j_frictionLimiter_angularProjector2 * body2_angularVelocity;

        Vf frictionDeltaImpulse = frictiondV * j_frictionLimiter_compInvMass;

        Vf reactionForce = j_normalLimiter_accumulatedImpulse;
        Vf accumulatedImpulse = j_frictionLimiter_accumulatedImpulse;

        Vf frictionForce = accumulatedImpulse + frictionDeltaImpulse;
        Vf reactionForceScaled = reactionForce * Vf::one(kFrictionCoefficient);

        Vf frictionForceAbs = abs(frictionForce);
        Vf reactionForceScaledSigned = flipsign(reactionForceScaled, frictionForce);
        Vf frictionDeltaImpulseAdjusted = reactionForceScaledSigned - accumulatedImpulse;

        frictionDeltaImpulse = select(frictionDeltaImpulse, frictionDeltaImpulseAdjusted, frictionForceAbs > reactionForceScaled);

        j_frictionLimiter_accumulatedImpulse += frictionDeltaImpulse;

        body1_velocityX += j_frictionLimiter_compMass1_linearX * frictionDeltaImpulse;
        body1_velocityY += j_frictionLimiter_compMass1_linearY * frictionDeltaImpulse;
        body1_angularVelocity += j_frictionLimiter_compMass1_angular * frictionDeltaImpulse;

        body2_velocityX += j_frictionLimiter_compMass2_linearX * frictionDeltaImpulse;
        body2_velocityY += j_frictionLimiter_compMass2_linearY * frictionDeltaImpulse;
        body2_angularVelocity += j_frictionLimiter_compMass2_angular * frictionDeltaImpulse;

        store(j_normalLimiter_accumulatedImpulse, &jointP.normalLimiter_accumulatedImpulse[iP]);
        store(j_frictionLimiter_accumulatedImpulse, &jointP.frictionLimiter_accumulatedImpulse[iP]);

        Vf cumulativeImpulse = max(abs(normalDeltaImpulse), abs(frictionDeltaImpulse));

        Vb productive = cumulativeImpulse > Vf::one(kProductiveImpulse);

        productive_any |= productive;

        body1_lastIteration = select(body1_lastIteration, iterationIndex0, productive);
        body2_lastIteration = select(body2_lastIteration, iterationIndex0, productive);

        body1_lastIterationf = bitcast(body1_lastIteration);
        body2_lastIterationf = bitcast(body2_lastIteration);

        storeindexed4(body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf,
            solveBodiesImpulse.data, jointP.body1Index + iP, sizeof(SolveBody));

        storeindexed4(body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf,
            solveBodiesImpulse.data, jointP.body2Index + iP, sizeof(SolveBody));
    }

    return any(productive_any);
}

template <int VN, int N>
NOINLINE bool Solver::SolveJointsDisplacementSoA(ContactJointPacked<N>* joint_packed, int jointBegin, int jointEnd, int iterationIndex)
{
    MICROPROFILE_SCOPEI("Physics", "SolveJointsDisplacementSoA", -1);

    typedef simd::VNf<VN> Vf;
    typedef simd::VNi<VN> Vi;
    typedef simd::VNb<VN> Vb;

    assert(jointBegin % VN == 0 && jointEnd % VN == 0);

    Vi iterationIndex0 = Vi::one(iterationIndex);
    Vi iterationIndex2 = Vi::one(iterationIndex - 2);

    Vb productive_any = Vb::zero();

    for (int jointIndex = jointBegin; jointIndex < jointEnd; jointIndex += VN)
    {
        int i = jointIndex;

        ContactJointPacked<N>& jointP = joint_packed[unsigned(i) / N];
        int iP = (VN == N) ? 0 : i & (N - 1);

        Vf body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf;
        Vf body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf;

        loadindexed4(body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf,
            solveBodiesDisplacement.data, jointP.body1Index + iP, sizeof(SolveBody));

        loadindexed4(body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf,
            solveBodiesDisplacement.data, jointP.body2Index + iP, sizeof(SolveBody));

        Vi body1_lastIteration = bitcast(body1_lastIterationf);
        Vi body2_lastIteration = bitcast(body2_lastIterationf);

        Vb body1_productive = body1_lastIteration > iterationIndex2;
        Vb body2_productive = body2_lastIteration > iterationIndex2;
        Vb body_productive = body1_productive | body2_productive;

        if (none(body_productive))
            continue;

        Vf j_normalLimiter_normalProjector1X = Vf::load(&jointP.normalLimiter_normalProjector1X[iP]);
        Vf j_normalLimiter_normalProjector1Y = Vf::load(&jointP.normalLimiter_normalProjector1Y[iP]);
        Vf j_normalLimiter_normalProjector2X = Vf::load(&jointP.normalLimiter_normalProjector2X[iP]);
        Vf j_normalLimiter_normalProjector2Y = Vf::load(&jointP.normalLimiter_normalProjector2Y[iP]);
        Vf j_normalLimiter_angularProjector1 = Vf::load(&jointP.normalLimiter_angularProjector1[iP]);
        Vf j_normalLimiter_angularProjector2 = Vf::load(&jointP.normalLimiter_angularProjector2[iP]);

        Vf j_normalLimiter_compMass1_linearX = Vf::load(&jointP.normalLimiter_compMass1_linearX[iP]);
        Vf j_normalLimiter_compMass1_linearY = Vf::load(&jointP.normalLimiter_compMass1_linearY[iP]);
        Vf j_normalLimiter_compMass2_linearX = Vf::load(&jointP.normalLimiter_compMass2_linearX[iP]);
        Vf j_normalLimiter_compMass2_linearY = Vf::load(&jointP.normalLimiter_compMass2_linearY[iP]);
        Vf j_normalLimiter_compMass1_angular = Vf::load(&jointP.normalLimiter_compMass1_angular[iP]);
        Vf j_normalLimiter_compMass2_angular = Vf::load(&jointP.normalLimiter_compMass2_angular[iP]);
        Vf j_normalLimiter_compInvMass = Vf::load(&jointP.normalLimiter_compInvMass[iP]);
        Vf j_normalLimiter_dstDisplacingVelocity = Vf::load(&jointP.normalLimiter_dstDisplacingVelocity[iP]);
        Vf j_normalLimiter_accumulatedDisplacingImpulse = Vf::load(&jointP.normalLimiter_accumulatedDisplacingImpulse[iP]);

        Vf dV = j_normalLimiter_dstDisplacingVelocity;

        dV -= j_normalLimiter_normalProjector1X * body1_velocityX;
        dV -= j_normalLimiter_normalProjector1Y * body1_velocityY;
        dV -= j_normalLimiter_angularProjector1 * body1_angularVelocity;

        dV -= j_normalLimiter_normalProjector2X * body2_velocityX;
        dV -= j_normalLimiter_normalProjector2Y * body2_velocityY;
        dV -= j_normalLimiter_angularProjector2 * body2_angularVelocity;

        Vf displacingDeltaImpulse = dV * j_normalLimiter_compInvMass;

        displacingDeltaImpulse = max(displacingDeltaImpulse, -j_normalLimiter_accumulatedDisplacingImpulse);

        body1_velocityX += j_normalLimiter_compMass1_linearX * displacingDeltaImpulse;
        body1_velocityY += j_normalLimiter_compMass1_linearY * displacingDeltaImpulse;
        body1_angularVelocity += j_normalLimiter_compMass1_angular * displacingDeltaImpulse;

        body2_velocityX += j_normalLimiter_compMass2_linearX * displacingDeltaImpulse;
        body2_velocityY += j_normalLimiter_compMass2_linearY * displacingDeltaImpulse;
        body2_angularVelocity += j_normalLimiter_compMass2_angular * displacingDeltaImpulse;

        j_normalLimiter_accumulatedDisplacingImpulse += displacingDeltaImpulse;

        store(j_normalLimiter_accumulatedDisplacingImpulse, &jointP.normalLimiter_accumulatedDisplacingImpulse[iP]);

        Vb productive = abs(displacingDeltaImpulse) > Vf::one(kProductiveImpulse);

        productive_any |= productive;

        body1_lastIteration = select(body1_lastIteration, iterationIndex0, productive);
        body2_lastIteration = select(body2_lastIteration, iterationIndex0, productive);

        // this is a bit painful :(
        body1_lastIterationf = bitcast(body1_lastIteration);
        body2_lastIterationf = bitcast(body2_lastIteration);

        storeindexed4(body1_velocityX, body1_velocityY, body1_angularVelocity, body1_lastIterationf,
            solveBodiesDisplacement.data, jointP.body1Index + iP, sizeof(SolveBody));

        storeindexed4(body2_velocityX, body2_velocityY, body2_angularVelocity, body2_lastIterationf,
            solveBodiesDisplacement.data, jointP.body2Index + iP, sizeof(SolveBody));
    }

    return any(productive_any);
}