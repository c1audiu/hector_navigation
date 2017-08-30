#include "hector_gps_calibration/hector_gps_calibration.h"
#include "hector_gps_calibration/transform_delta_cost_functor.h"
#include "hector_gps_calibration/angle_local_parameterization.h"

#include <ceres/ceres.h>

#include <iostream>
#include <fstream>

GPSCalibration::GPSCalibration(ros::NodeHandle &nh)
  : tf_listener(tf_buffer),
    translation_{{0,0}},
    rotation_(0.0)
{
  nh.param<double>("translation_x", translation_[0], 0.0);
  nh.param<double>("translation_y", translation_[1], 0.0);

  nh.param<double>("orientation", rotation_, 0.0);

  nh.param<bool>("write_debug_file", write_debug_file_, false);

  ROS_INFO("Initial GPS transformation: \n t: %f %f \n r: %f", translation_[0], translation_[1], rotation_);

  nav_sat_sub_ = nh.subscribe("/odom_gps", 10, &GPSCalibration::navSatCallback, this);
  optimize_sub_ = nh.subscribe("gps/run_optimization", 10, &GPSCalibration::navSatCallback, this);
  /*TEST
    Eigen::Matrix<double, 3, 1> pos_gps(0, 0, 10);
    gps_poses_.emplace_back(pos_gps);
    pos_gps = Eigen::Matrix<double, 3, 1>(0, 1, 10);
    gps_poses_.emplace_back(pos_gps);
    pos_gps = Eigen::Matrix<double, 3, 1>(0, 2, 10);
    gps_poses_.emplace_back(pos_gps);
    pos_gps = Eigen::Matrix<double, 3, 1>(0, 3, 10);
    gps_poses_.emplace_back(pos_gps);

    Eigen::Matrix<double, 3, 1> pos_world(0.1, 0, 0);
    world_poses_.emplace_back(pos_world);
    pos_world = Eigen::Matrix<double, 3, 1>(0, 1, 0.1);
    world_poses_.emplace_back(pos_world);
    pos_world = Eigen::Matrix<double, 3, 1>(0, 2, 0);
    world_poses_.emplace_back(pos_world);
    pos_world = Eigen::Matrix<double, 3, 1>(0, 3, 0);
    world_poses_.emplace_back(pos_world);
    optimize();*/

  wall_timers_.push_back(nh.createWallTimer(ros::WallDuration(0.1), &GPSCalibration::publishTF, this));
}

void GPSCalibration::navSatCallback(nav_msgs::Odometry msg)
{
  if(msg.header.frame_id != "navsat_link") {
    ROS_WARN("Expecting odometry for navsat_link, received: %s", msg.header.frame_id.c_str());
  }
    Eigen::Matrix<double, 2, 1> pos_gps(msg.pose.pose.position.x,
                                        msg.pose.pose.position.y);

  geometry_msgs::TransformStamped transformStamped;
  try{
    transformStamped = tf_buffer.lookupTransform("world", "navsat_link",
                                                 msg.header.stamp, ros::Duration(3.0));
  }
  catch (tf2::TransformException &ex) {
    ROS_WARN("%s",ex.what());
    return;
  }

  Eigen::Matrix<double, 2, 1> pos_world(transformStamped.transform.translation.x,
                                        transformStamped.transform.translation.y);

  gps_poses_.emplace_back(pos_gps);
  world_poses_.emplace_back(pos_world);

  if((world_poses_.size() % 10 == 0) && world_poses_.size() > 0)
    optimize();

}

void GPSCalibration::optimizeCallback(std_msgs::Empty msg)
{
  optimize();
}


void GPSCalibration::optimize()
{
  int i = 0;

  ceres::Problem problem;

  for(i = 0; i < world_poses_.size(); ++i)
  {
    problem.AddResidualBlock(
          new ceres::AutoDiffCostFunction<TransformDeltaCostFunctor,
          2, 2, 1>(
            new TransformDeltaCostFunctor(world_poses_[i],
                                            gps_poses_[i])),
          nullptr, translation_.data(), &rotation_);
  }
  //problem.SetParameterization(&rotation_, new ceres::QuaternionParameterization());

  ceres::LocalParameterization* angle_local_parameterization =
      ceres::examples::AngleLocalParameterization::Create();

  problem.SetParameterization(&rotation_, angle_local_parameterization);


  ceres::Solver::Options options;
  options.linear_solver_type = ceres::DENSE_QR;
  //  options.minimizer_progress_to_stdout = true;
  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);
  if(summary.termination_type != ceres::TerminationType::CONVERGENCE)
    ROS_WARN("%s", summary.FullReport().c_str());
  ROS_INFO("Translation %f %f", translation_[0], translation_[1]);
  ROS_INFO("Rotation %f", rotation_);

  if(write_debug_file_)
  {
    std::ofstream myfile;
    myfile.open ("gps_alignment_solution.csv");
    const Rigid3<double> transform = Rigid3<double>(
          Eigen::Matrix<double, 3, 1>(translation_[0], translation_[1], 0.0),
        Eigen::Quaternion<double>(std::cos(rotation_/2.0), 0.0, 0.0,
        std::sin(rotation_/2.0)));

    myfile <<"gps_x"<<","<<"gps_y"<<","
          <<"world_x"<<","<<"world_y"<<"\n";
    for(size_t i = 0; i<gps_poses_.size(); ++i)
    {
      const Eigen::Matrix<double, 3, 1> pos_world_gps = transform * Eigen::Matrix<double, 3, 1>(world_poses_[i][0], world_poses_[i][1], 0.0);
      myfile << std::setprecision (15)<< gps_poses_[i][0]<<","<<gps_poses_[i][1]<<","
            <<pos_world_gps[0]<<","<<pos_world_gps[1]<<"\n";
    }
    myfile.close();
  }
}

void GPSCalibration::publishTF(const ::ros::WallTimerEvent& unused_timer_event)
{

  geometry_msgs::TransformStamped transformStamped;
  transformStamped.header.stamp = ros::Time::now();
  transformStamped.header.frame_id = "utm";
  transformStamped.child_frame_id = "world";
  transformStamped.transform.translation.x = translation_[0];
  transformStamped.transform.translation.y = translation_[1];
  transformStamped.transform.translation.z = 0;
  transformStamped.transform.rotation.w = std::cos(rotation_/2.0);
  transformStamped.transform.rotation.x = 0.0;
  transformStamped.transform.rotation.y = 0.0;
  transformStamped.transform.rotation.z = std::sin(rotation_/2.0);

  tf_broadcaster.sendTransform(transformStamped);

}
