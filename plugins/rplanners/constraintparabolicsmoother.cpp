// -*- coding: utf-8 -*-
// Copyright (C) 2012-2013 Rosen Diankov <rosen.diankov@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include "plugindefs.h"
#include <fstream>

#include <openrave/planningutils.h>

#include "ParabolicPathSmooth/DynamicPath.h"
#include "mergewaypoints.h"


namespace ParabolicRamp = ParabolicRampInternal;

class ConstraintParabolicSmoother : public PlannerBase, public ParabolicRamp::FeasibilityCheckerBase, public ParabolicRamp::RandomNumberGeneratorBase
{
    struct LinkConstraintInfo
    {
        KinBody::LinkPtr plink;
        AABB ablocal; // local aabb of the link
    };

public:
    ConstraintParabolicSmoother(EnvironmentBasePtr penv, std::istream& sinput) : PlannerBase(penv)
    {
        __description = ":Interface Author: Rosen Diankov\nConstraint-based smoothing with `Indiana University Intelligent Motion Laboratory <http://www.iu.edu/~motion/software.html>`_ parabolic smoothing library (Kris Hauser).\n\n**Note:** The original trajectory will not be preserved at all, don't use this if the robot has to hit all points of the trajectory.\n";
        //_distancechecker = RaveCreateCollisionChecker(penv, "pqp");
        //OPENRAVE_ASSERT_FORMAT0(!!_distancechecker, "need pqp distance checker", ORE_Assert);
        _uniformsampler = RaveCreateSpaceSampler(GetEnv(),"mt19937");
        OPENRAVE_ASSERT_FORMAT0(!!_uniformsampler, "need mt19937 space samplers", ORE_Assert);
    }

    virtual bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr params)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        _parameters.reset(new ConstraintTrajectoryTimingParameters());
        _parameters->copy(params);
        _probot = pbase;
        return _InitPlan();
    }

    virtual bool InitPlan(RobotBasePtr pbase, std::istream& isParameters)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        _parameters.reset(new ConstraintTrajectoryTimingParameters());
        isParameters >> *_parameters;
        _probot = pbase;
        return _InitPlan();
    }

    bool _InitPlan()
    {
        if( _parameters->_nMaxIterations <= 0 ) {
            _parameters->_nMaxIterations = 100;
        }
        _bUsePerturbation = true;
//        if( !_distancechecker->InitEnvironment() ) {
//            return false;
//        }
        _listCheckLinks.clear();
        if( _parameters->maxlinkspeed > 0 || _parameters->maxlinkaccel ) {
            // extract links from _parameters->_configurationspecification?
            //_listCheckLinks
            //_parameters->_configurationspecification
        }
        return true;
    }

    virtual PlannerParametersConstPtr GetParameters() const {
        return _parameters;
    }

    virtual PlannerStatus PlanPath(TrajectoryBasePtr ptraj)
    {
        BOOST_ASSERT(!!_parameters && !!ptraj);
        if( ptraj->GetNumWaypoints() < 2 ) {
            return PS_Failed;
        }

        RobotBase::RobotStateSaverPtr statesaver;
        if( !!_probot ) {
            statesaver.reset(new RobotBase::RobotStateSaver(_probot));
        }

        uint32_t basetime = utils::GetMilliTime();

        ConfigurationSpecification posspec = _parameters->_configurationspecification;
        _setstatefn = posspec.GetSetFn(GetEnv());
        ConfigurationSpecification velspec = posspec.ConvertToVelocitySpecification();
        _setvelstatefn = velspec.GetSetFn(GetEnv());
        ConfigurationSpecification timespec;
        timespec.AddDeltaTimeGroup();

        vector<ParabolicRamp::Vector> path;
        path.reserve(ptraj->GetNumWaypoints());
        vector<dReal> vtrajpoints;
        ptraj->GetWaypoints(0,ptraj->GetNumWaypoints(),vtrajpoints,posspec);


        try {
            ParabolicRamp::Vector tol = _parameters->_vConfigResolution;
            FOREACH(it,tol) {
                *it *= _parameters->_pointtolerance;
            }
            ParabolicRamp::RampFeasibilityChecker checker(this,tol);

            _bUsePerturbation = true;
            std::list<ParabolicRamp::ParabolicRampND> ramps;

            std::vector<ConfigurationSpecification::Group>::const_iterator itcompatposgroup = ptraj->GetConfigurationSpecification().FindCompatibleGroup(posspec._vgroups.at(0), false);
            OPENRAVE_ASSERT_FORMAT(itcompatposgroup != ptraj->GetConfigurationSpecification()._vgroups.end(), "failed to find group %s in passed in trajectory", posspec._vgroups.at(0).name, ORE_InvalidArguments);
            if (_parameters->_hastimestamps && itcompatposgroup->interpolation == "quadratic" ) {
                // assumes that the traj has velocity data and is consistent, so convert the original trajectory in a sequence of ramps, and preserve velocity
                vector<dReal> x0, x1, dx0, dx1, ramptime;
                ptraj->GetWaypoint(0,x0,posspec);
                ptraj->GetWaypoint(0,dx0,velspec);
                for(size_t i=0; i+1<ptraj->GetNumWaypoints(); i++) {
                    ptraj->GetWaypoint(i+1,ramptime,timespec);
                    ptraj->GetWaypoint(i+1,x1,posspec);
                    ptraj->GetWaypoint(i+1,dx1,velspec);
                    ramps.push_back(ParabolicRamp::ParabolicRampND());
                    ramps.back().SetPosVelTime(x0,dx0,x1,dx1,ramptime.at(0));
                    /*if( !checker.Check(ramps.back())) {
                        RAVELOG_WARN("ramp %d failed\n", i);
                        ramps.back().SetPosVelTime(x0,dx0,x1,dx1,2.0*ramptime.at(0));
                        checker.Check(ramps.back());
                        }*/

                    x0.swap(x1);
                    dx0.swap(dx1);
                }
            }
            else {
                // assumes all the waypoints are joined by linear segments

                ParabolicRamp::Vector q(_parameters->GetDOF());
                for(size_t i = 0; i < ptraj->GetNumWaypoints(); ++i) {
                    std::copy(vtrajpoints.begin()+i*_parameters->GetDOF(),vtrajpoints.begin()+(i+1)*_parameters->GetDOF(),q.begin());
                    if( path.size() >= 2 ) {
                        // check if collinear by taking angle
                        const ParabolicRamp::Vector& x0 = path[path.size()-2];
                        const ParabolicRamp::Vector& x1 = path[path.size()-1];
                        dReal dotproduct=0,x0length2=0,x1length2=0;
                        for(size_t i = 0; i < q.size(); ++i) {
                            dReal dx0=x0[i]-q[i];
                            dReal dx1=x1[i]-q[i];
                            dotproduct += dx0*dx1;
                            x0length2 += dx0*dx0;
                            x1length2 += dx1*dx1;
                        }
                        if( RaveFabs(dotproduct * dotproduct - x0length2*x1length2) < 1e-8 ) {
                            path.back() = q;
                            continue;
                        }
                    }
                    // check if the point is not the same as the previous point
                    if( path.size() > 0 ) {
                        dReal d = 0;
                        for(size_t i = 0; i < q.size(); ++i) {
                            d += RaveFabs(q[i]-path.back().at(i));
                        }
                        if( d <= q.size()*std::numeric_limits<dReal>::epsilon() ) {
                            continue;
                        }
                    }
                    path.push_back(q);
                }

                SetMilestones(ramps, path);
            }


            dReal totaltime=0;
            FOREACH(itramp, ramps) {
                totaltime += itramp->endTime;
            }





            ///////////////////////////////////////////////////////////////////////////

            //Tests here
            vector<dReal> q0,v0,q1,v1,q2,v2,q3,v3,t0,t1,t2;
            vector<dReal> amax = _parameters->_vConfigAccelerationLimit;
            vector<dReal> vmax = _parameters->_vConfigVelocityLimit;
            vector<dReal> qmin = _parameters->_vConfigLowerLimit;
            vector<dReal> qmax = _parameters->_vConfigUpperLimit;


            printf("\n-----------------------------------\n");

            std::list<ParabolicRamp::ParabolicRampND> resramps;
            BreakIntoUnitaryRamps(ramps,resramps);
            TimeScale(resramps,2);

            ParabolicRamp::ParabolicRampND ramp0,ramp1,ramp2,resramp0,resramp1;

            int i =0;
            FOREACH(itramp, resramps) {
                printf("ramp duration=%f\n",itramp->endTime);
                switch(i) {
                case 0:
                    ramp0 = *itramp;
                    break;
                case 1:
                    ramp1 = *itramp;
                    break;
                case 2:
                    ramp2 = *itramp;
                    break;
                }
                i++;
            }

            printf("t0=%f,t1=%f,t2=%f\n",(float)ramp0.endTime,(float) ramp1.endTime,(float) ramp2.endTime);



            bool res = MergeWaypoints(ramp0,ramp1,ramp2,resramp0,resramp1,qmin,qmax,vmax,amax);

            printf("\n-----------------------------------\n");
            cout << res << '\n';
            cout << resramp0.endTime << '\n';
            cout << resramp1.endTime << '\n';



            //////////////////////////////////////////////////////////////////////////////












            RAVELOG_DEBUG(str(boost::format("initial path size=%d, duration=%f, pointtolerance=%f")%path.size()%totaltime%_parameters->_pointtolerance));

            _progress._iteration=0;
            int numshortcuts=0;
            numshortcuts = Shortcut(ramps, _parameters->_nMaxIterations, checker, this);
            if( numshortcuts < 0 ) {
                // interrupted
                return PS_Interrupted;
            }

            BOOST_ASSERT( ramps.size() > 0 );

            ConfigurationSpecification newspec = posspec;
            newspec.AddDerivativeGroups(1,true);
            int waypointoffset = newspec.AddGroup("iswaypoint", 1, "next");

            int timeoffset=-1;
            FOREACH(itgroup,newspec._vgroups) {
                if( itgroup->name == "deltatime" ) {
                    timeoffset = itgroup->offset;
                }
                else if( velspec.FindCompatibleGroup(*itgroup) != velspec._vgroups.end() ) {
                    itgroup->interpolation = "linear";
                }
                else if( posspec.FindCompatibleGroup(*itgroup) != posspec._vgroups.end() ) {
                    itgroup->interpolation = "quadratic";
                }
            }

            // have to write to another trajectory
            if( !_dummytraj || _dummytraj->GetXMLId() != ptraj->GetXMLId() ) {
                _dummytraj = RaveCreateTrajectory(GetEnv(), ptraj->GetXMLId());
            }
            _dummytraj->Init(newspec);

            totaltime=0;
            FOREACH(itramp, ramps) {
                totaltime += itramp->endTime;
            }

            // separate all the acceleration switches into individual points
            vtrajpoints.resize(newspec.GetDOF());
            ConfigurationSpecification::ConvertData(vtrajpoints.begin(),newspec,ramps.front().x0.begin(),posspec,1,GetEnv(),true);
            ConfigurationSpecification::ConvertData(vtrajpoints.begin(),newspec,ramps.front().dx0.begin(),velspec,1,GetEnv(),false);
            vtrajpoints.at(waypointoffset) = 1;
            vtrajpoints.at(timeoffset) = 0;
            _dummytraj->Insert(_dummytraj->GetNumWaypoints(),vtrajpoints);
            vector<dReal> vswitchtimes;
            ParabolicRamp::Vector vconfig;
            FOREACH(itrampnd,ramps) {
                // double-check the current ramps
                if(!itrampnd->constraintchecked ) {
                    // part of original trajectory which might not have been processed with perterbations, so ignore them
                    _bUsePerturbation = false;
                    if( !checker.Check(*itrampnd)) {
                        // try to time scale, perhaps collision and dynamics will change
                        // go all the way up to 2.0 multiplier: 1.05*1.1*1.15*1.2*1.25 ~= 2
                        dReal oldtime = itrampnd->endTime;
                        bool bSuccess = false;
                        dReal mult = 1.05;
                        for(size_t idilate = 0; idilate < 5; ++idilate ) {
                            itrampnd->Dilate(mult);
                            if( checker.Check(*itrampnd) ) {
                                bSuccess = true;
                                break;
                            }
                            mult += 0.05;
                        }
                        if( !bSuccess ) {
                            throw OPENRAVE_EXCEPTION_FORMAT0("original ramp is in collision!", ORE_Assert);
                        }
                        totaltime += itrampnd->endTime-oldtime;
                    }
                    _bUsePerturbation = true; // re-enable
                    _progress._iteration+=1;
                    if( _CallCallbacks(_progress) == PA_Interrupt ) {
                        return PS_Interrupted;
                    }
                }

                vswitchtimes.resize(0);
                vswitchtimes.push_back(itrampnd->endTime);
                if( _parameters->_outputaccelchanges ) {
                    FOREACHC(itramp,itrampnd->ramps) {
                        vector<dReal>::iterator it;
                        if( itramp->tswitch1 != 0 ) {
                            it = lower_bound(vswitchtimes.begin(),vswitchtimes.end(),itramp->tswitch1);
                            if( *it != itramp->tswitch1) {
                                vswitchtimes.insert(it,itramp->tswitch1);
                            }
                        }
                        if( itramp->tswitch1 != itramp->tswitch2 && itramp->tswitch2 != 0 ) {
                            it = lower_bound(vswitchtimes.begin(),vswitchtimes.end(),itramp->tswitch2);
                            if( *it != itramp->tswitch2 ) {
                                vswitchtimes.insert(it,itramp->tswitch2);
                            }
                        }
                    }
                }
                vtrajpoints.resize(newspec.GetDOF()*vswitchtimes.size());
                vector<dReal>::iterator ittargetdata = vtrajpoints.begin();
                dReal prevtime = 0;
                for(size_t i = 0; i < vswitchtimes.size(); ++i) {
                    itrampnd->Evaluate(vswitchtimes[i],vconfig);
                    ConfigurationSpecification::ConvertData(ittargetdata,newspec,vconfig.begin(),posspec,1,GetEnv(),true);
                    itrampnd->Derivative(vswitchtimes[i],vconfig);
                    ConfigurationSpecification::ConvertData(ittargetdata,newspec,vconfig.begin(),velspec,1,GetEnv(),false);
                    *(ittargetdata+timeoffset) = vswitchtimes[i]-prevtime;
                    *(ittargetdata+waypointoffset) = dReal(i+1==vswitchtimes.size());
                    ittargetdata += newspec.GetDOF();
                    prevtime = vswitchtimes[i];
                }
                _dummytraj->Insert(_dummytraj->GetNumWaypoints(),vtrajpoints);
            }

            BOOST_ASSERT(RaveFabs(totaltime-_dummytraj->GetDuration())<0.001);
            RAVELOG_DEBUG(str(boost::format("after shortcutting %d times: path waypoints=%d, traj waypoints=%d, traj time=%fs")%numshortcuts%ramps.size()%_dummytraj->GetNumWaypoints()%totaltime));
            ptraj->Swap(_dummytraj);
        }
        catch (const std::exception& ex) {
            if( IS_DEBUGLEVEL(Level_Verbose) ) {
                string filename = str(boost::format("%s/failedsmoothing%d.xml")%RaveGetHomeDirectory()%(RaveRandomInt()%10000));
                RAVELOG_WARN(str(boost::format("parabolic planner failed: %s, writing original trajectory to %s")%ex.what()%filename));
                ofstream f(filename.c_str());
                f << std::setprecision(std::numeric_limits<dReal>::digits10+1);
                ptraj->serialize(f);
            }
            else {
                RAVELOG_WARN(str(boost::format("parabolic planner failed: %s")%ex.what()));
            }
            return PS_Failed;
        }
        RAVELOG_DEBUG(str(boost::format("path optimizing - computation time=%fs\n")%(0.001f*(float)(utils::GetMilliTime()-basetime))));
        return PS_HasSolution;
    }

    inline bool SolveMinTimeWithConstraints(const ParabolicRamp::Vector& x0,const ParabolicRamp::Vector& dx0,const ParabolicRamp::Vector& x1,const ParabolicRamp::Vector& dx1, dReal curtime, ParabolicRamp::RampFeasibilityChecker& check, std::list<ParabolicRamp::ParabolicRampND>& rampsout)
    {
        __tempramps1d.resize(0);
        dReal mintime = ParabolicRamp::SolveMinTimeBounded(x0,dx0,x1,dx1, _parameters->_vConfigAccelerationLimit, _parameters->_vConfigVelocityLimit, _parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit, __tempramps1d, _parameters->_multidofinterp);
        if(mintime < 0 || mintime > curtime ) {
            return false;
        }
        rampsout.clear();
        CombineRamps(__tempramps1d,rampsout);
        bool feas=true;
        FOREACH(itramp, rampsout) {
            if(!check.Check(*itramp)) {
                feas=false;
                break;
            }
        }
        return feas;
    }

    void SetMilestones(std::list<ParabolicRamp::ParabolicRampND>& ramps, const vector<ParabolicRamp::Vector>& x)
    {
        ramps.clear();
        if(x.size()==1) {
            ramps.push_back(ParabolicRamp::ParabolicRampND());
            ramps.front().SetConstant(x[0]);
        }
        else if( !x.empty() ) {
            ParabolicRamp::Vector zero(x[0].size(),0.0);
            for(size_t i=0; i+1<x.size(); i++) {
                ramps.push_back(ParabolicRamp::ParabolicRampND());
                ramps.back().x0 = x[i];
                ramps.back().x1 = x[i+1];
                ramps.back().dx0 = zero;
                ramps.back().dx1 = zero;
                bool res=ramps.back().SolveMinTimeLinear(_parameters->_vConfigAccelerationLimit, _parameters->_vConfigVelocityLimit);
                PARABOLIC_RAMP_ASSERT(res && ramps.back().IsValid());
            }
        }
    }

    int Shortcut(std::list<ParabolicRamp::ParabolicRampND>&ramps, int numIters,ParabolicRamp::RampFeasibilityChecker& check,ParabolicRamp::RandomNumberGeneratorBase* rng)
    {
        int shortcuts = 0;
        std::vector<dReal> rampStartTime; rampStartTime.resize(ramps.size());
        dReal endTime=0;
        int i = 0;
        FOREACH(itramp, ramps) {
            rampStartTime[i++] = endTime;
            endTime += itramp->endTime;
        }
        ParabolicRamp::Vector x0,x1,dx0,dx1;
        std::list<ParabolicRamp::ParabolicRampND> intermediate;
        std::list<ParabolicRamp::ParabolicRampND>::iterator itramp1, itramp2;
        for(int iters=0; iters<numIters; iters++) {
            dReal t1=rng->Rand()*endTime,t2=rng->Rand()*endTime;
            if( iters == 0 ) {
                t1 = 0;
                t2 = endTime;
            }
            if(t1 > t2) {
                std::swap(t1,t2);
            }
            int i1 = std::upper_bound(rampStartTime.begin(),rampStartTime.end(),t1)-rampStartTime.begin()-1;
            int i2 = std::upper_bound(rampStartTime.begin(),rampStartTime.end(),t2)-rampStartTime.begin()-1;
            if(i1 == i2) {
                continue;
            }

            _progress._iteration+=1;
            if( _CallCallbacks(_progress) == PA_Interrupt ) {
                return -1;
            }

            //same ramp
            dReal u1 = t1-rampStartTime[i1];
            dReal u2 = t2-rampStartTime[i2];
            itramp1 = ramps.begin();
            advance(itramp1, i1);
            itramp2 = ramps.begin();
            advance(itramp2, i2);
            PARABOLIC_RAMP_ASSERT(u1 >= 0);
            PARABOLIC_RAMP_ASSERT(u1 <= itramp1->endTime+ParabolicRamp::EpsilonT);
            PARABOLIC_RAMP_ASSERT(u2 >= 0);
            PARABOLIC_RAMP_ASSERT(u2 <= itramp2->endTime+ParabolicRamp::EpsilonT);
            u1 = min(u1,itramp1->endTime);
            u2 = min(u2,itramp2->endTime);
            itramp1->Evaluate(u1,x0);
            itramp2->Evaluate(u2,x1);
            itramp1->Derivative(u1,dx0);
            itramp2->Derivative(u2,dx1);

            bool res=SolveMinTimeWithConstraints(x0,dx0,x1,dx1,t2-t1, check, intermediate);
            if(!res) {
                continue;
            }
            // no idea what a good mintimestep is... _parameters->_fStepLength*0.5?
            dReal newramptime = 0;
            FOREACH(itramp, intermediate) {
                newramptime += itramp->endTime;
            }
            if( newramptime+_parameters->_fStepLength > t2-t1 ) {
                // reject since it didn't make significant improvement
                RAVELOG_VERBOSE("shortcut iter=%d rejected time=%fs\n", iters, endTime-(t2-t1)+newramptime);
                continue;
            }

            //perform shortcut
            shortcuts++;
            itramp1->TrimBack(itramp1->endTime-u1);
            itramp1->x1 = intermediate.front().x0;
            itramp1->dx1 = intermediate.front().dx0;
            itramp2->TrimFront(u2);
            itramp2->x0 = intermediate.back().x1;
            itramp2->dx0 = intermediate.back().dx1;

            //replace intermediate ramps
            ++itramp1;
            ramps.erase(itramp1, itramp2);
            ramps.splice(itramp2, intermediate);

            if( IS_DEBUGLEVEL(Level_Verbose) ) {
                //check for consistency
                itramp1 = ramps.begin();
                itramp2 = ramps.begin(); ++itramp2;
                while(itramp2 != ramps.end() ) {
                    PARABOLIC_RAMP_ASSERT(itramp1->x1 == itramp2->x0);
                    PARABOLIC_RAMP_ASSERT(itramp1->dx1 == itramp2->dx0);
                    itramp1=itramp2;
                    ++itramp2;
                }
            }

            //revise the timing
            rampStartTime.resize(ramps.size());
            endTime=0;
            i = 0;
            FOREACH(itramp, ramps) {
                rampStartTime[i++] = endTime;
                endTime += itramp->endTime;
            }
        }
        return shortcuts;
    }

    virtual bool ConfigFeasible(const ParabolicRamp::Vector& a, const ParabolicRamp::Vector& da)
    {
        if( _bUsePerturbation ) {
            // have to also test with tolerances!
            boost::array<dReal,3> perturbations = {{ 0,_parameters->_pointtolerance,-_parameters->_pointtolerance}};
            ParabolicRamp::Vector anew(a.size());
            FOREACH(itperturbation,perturbations) {
                for(size_t i = 0; i < a.size(); ++i) {
                    anew[i] = a[i] + *itperturbation * _parameters->_vConfigResolution.at(i);
                }
                (*_setstatefn)(anew);
                if( _parameters->CheckPathAllConstraints(a,a,da,da,0,IT_OpenStart) <= 0 ) {
                    return false;
                }
            }
        }
        else {
            //_parameters->_setstatefn(a);
            if( _parameters->CheckPathAllConstraints(a,a, da, da, 0, IT_OpenStart) <= 0 ) {
                return false;
            }
        }
        return true;
    }

    /** \brief return true if all the links in _listCheckLinks satisfy the acceleration and velocity constraints

       |w x (R x_i) + v| <= thresh

     */
    virtual bool _CheckConstraintLinks() const {
        FOREACHC(itinfo, _listCheckLinks) {
        }
        return true;
    }

    virtual bool SegmentFeasible(const ParabolicRamp::Vector& a,const ParabolicRamp::Vector& b, const ParabolicRamp::Vector& da,const ParabolicRamp::Vector& db, dReal timeelapsed)
    {
        if( _bUsePerturbation ) {
            // have to also test with tolerances!
            boost::array<dReal,3> perturbations = {{ 0,_parameters->_pointtolerance,-_parameters->_pointtolerance}};
            ParabolicRamp::Vector anew(a.size()), bnew(b.size());
            FOREACH(itperturbation,perturbations) {
                for(size_t i = 0; i < a.size(); ++i) {
                    anew[i] = a[i] + *itperturbation * _parameters->_vConfigResolution.at(i);
                    bnew[i] = b[i] + *itperturbation * _parameters->_vConfigResolution.at(i);
                }
                //(*_setstatefn)(anew);
                if( _parameters->CheckPathAllConstraints(anew,bnew,da, db, timeelapsed, IT_OpenStart) <= 0 ) {
                    return false;
                }
            }
        }
        else {
            //_parameters->_setstatefn(a);
            if( _parameters->CheckPathAllConstraints(a,b,da, db, timeelapsed, IT_OpenStart) <= 0 ) {
                return false;
            }
        }
        return true;
    }

    /*
       def ComputeDistanceToEnvironment(const std::vector<dReal>& vdofvalues):
        """robot state is not saved and environment is not locked
        """
        env=self.robot.GetEnv()
        pqpchecker = RaveCreateCollisionChecker(env,'pqp')
        if pqpchecker is not None:
            oldchecker = env.GetCollisionChecker()
            try:
                env.SetCollisionChecker(pqpchecker)
                with CollisionOptionsStateSaver(pqpchecker, CollisionOptions.Distance|CollisionOptions.Contacts):
                    self.robot.GetGrabbed()
                    checklinks = set()
                    for manip in self.robot.GetManipulators():
                        for childlink in  manip.GetChildLinks():
                            checklinks.add(childlink)
                    for link in self.robot.GetLinks():
                        if not link in checklinks:
                            link.Enable(False)
                    report=CollisionReport()
                    distancevalues=[]
                    for dofvalues in dofvaluess:
                        self.robot.SetDOFValues(dofvalues)
                        check = env.CheckCollision(self.robot, report=report)
                        distancevalues.append([report.minDistance,report.contacts[0]])
                    return distancevalues

            finally:
                env.SetCollisionChecker(oldchecker)
     */

    virtual bool NeedDerivativeForFeasibility()
    {
        return _parameters->maxlinkspeed > 0 || _parameters->maxlinkaccel > 0 || _parameters->velocitydistancethresh > 0;
    }

    virtual ParabolicRamp::Real Rand()
    {
        return _uniformsampler->SampleSequenceOneReal(IT_OpenEnd);
    }

protected:
    ConstraintTrajectoryTimingParametersPtr _parameters;
    SpaceSamplerBasePtr _uniformsampler;
    RobotBasePtr _probot;
    CollisionCheckerBasePtr _distancechecker;
    boost::shared_ptr<ConfigurationSpecification::SetConfigurationStateFn> _setstatefn, _setvelstatefn;
    //boost::shared_ptr<ConfigurationSpecification::GetConfigurationStateFn> _getstatefn, _getvelstatefn;

    std::list< LinkConstraintInfo > _listCheckLinks;
    TrajectoryBasePtr _dummytraj;
    bool _bUsePerturbation;
    PlannerProgress _progress;

private:
    std::vector<std::vector<ParabolicRamp::ParabolicRamp1D> > __tempramps1d;
};


PlannerBasePtr CreateConstraintParabolicSmoother(EnvironmentBasePtr penv, std::istream& sinput)
{
    return PlannerBasePtr(new ConstraintParabolicSmoother(penv,sinput));
}

#ifdef RAVE_REGISTER_BOOST
#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()
BOOST_TYPEOF_REGISTER_TYPE(ParabolicRamp::ParabolicRamp1D)
BOOST_TYPEOF_REGISTER_TYPE(ParabolicRamp::ParabolicRampND)
#endif
