// -*- mode:c++; fill-column: 100; -*-

#include "vesc_driver.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <sstream>

#include "boost/bind.hpp"
#include "f1tenth_course/CarStatusMsg.h"
#include "f1tenth_course/VescStateStamped.h"
#include "f1tenth_course/AckermannCurvatureDriveMsg.h"
#include "nav_msgs/Odometry.h"

#include "config_reader/config_reader.h"
#include "shared/math/math_util.h"

static const bool kDebug = true;
static const float kCommandRate = 20;
static const float kCommandInterval = 1.0 / kCommandRate;

CONFIG_FLOAT(speed_to_erpm_gain_, "speed_to_erpm_gain");
CONFIG_FLOAT(speed_to_erpm_offset_, "speed_to_erpm_offset");

CONFIG_FLOAT(steering_to_servo_gain_, "steering_angle_to_servo_gain");
CONFIG_FLOAT(steering_to_servo_offset_, "steering_angle_to_servo_offset");

CONFIG_FLOAT(wheelbase_, "wheelbase");

CONFIG_FLOAT(erpm_speed_limit_, "erpm_speed_limit");
CONFIG_FLOAT(servo_min_, "servo_min");
CONFIG_FLOAT(servo_max_, "servo_max");

CONFIG_STRING(serial_port_, "serial_port");

config_reader::ConfigReader reader({
  "config/car.lua",
  "config/vesc.lua"
});

using f1tenth_course::CarStatusMsg;
using f1tenth_course::VescStateStamped;

namespace {

float mux_drive_speed_ = 0;
float mux_steering_angle_ = 0;


VescStateStamped state_msg_;
CarStatusMsg car_status_msg_;
}  // namespace

namespace vesc_driver
{

VescDriver::VescDriver(ros::NodeHandle nh,
                       ros::NodeHandle private_nh) :
    vesc_(std::string(),
          boost::bind(&VescDriver::vescPacketCallback, this, _1),
          boost::bind(&VescDriver::vescErrorCallback, this, _1)),
    driver_mode_(MODE_INITIALIZING),
    drive_mode_(kStoppedDrive),
    fw_version_major_(-1),
    fw_version_minor_(-1),
    t_last_command_(0),
    t_last_joystick_(0) {
  state_msg_.header.seq = 0;
  state_msg_.header.frame_id = "base_link";
  car_status_msg_.header = state_msg_.header;

  odom_msg_.header.stamp = ros::Time::now();
  odom_msg_.header.frame_id = "odom";
  odom_msg_.child_frame_id = "base_link";

  odom_msg_.twist.twist.linear.x = 0;
  odom_msg_.twist.twist.linear.y = 0;
  odom_msg_.twist.twist.linear.z = 0;
  odom_msg_.twist.twist.angular.x = 0;
  odom_msg_.twist.twist.angular.y = 0;
  odom_msg_.twist.twist.angular.z = 0;
  odom_msg_.twist.covariance =
                {0.001, 0.0, 0.0, 0.0, 0.0, 0.0,
                  0.0, 0.001, 0.0, 0.0, 0.0, 0.0,
                  0.0, 0.0, 0.001, 0.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 1000000.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 1000000.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0, 0.03};

  odom_msg_.pose.pose.position.x = 0;
  odom_msg_.pose.pose.position.y = 0;
  odom_msg_.pose.pose.position.z = 0;
  odom_msg_.pose.covariance =
                {0.001, 0.0, 0.0, 0.0, 0.0, 0.0,
                  0.0, 0.001, 0.0, 0.0, 0.0, 0.0,
                  0.0, 0.0, 1000000.0 , 0.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 1000000.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 1000000.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0, 0.03};
  odom_msg_.pose.pose.orientation.w = 1;
  odom_msg_.pose.pose.orientation.x = 0;
  odom_msg_.pose.pose.orientation.y = 0;
  odom_msg_.pose.pose.orientation.z = 0;


  // attempt to connect to the serial port
  try {
    if (kDebug) printf("CONNECT\n");
    vesc_.connect(serial_port_);
  } catch (SerialException e) {
    fprintf(stderr, "Failed to connect to the VESC, %s.", e.what());
    ros::shutdown();
    exit(1);
    return;
  }
  if (kDebug) printf("CONNECTED\n");
  state_pub_ = nh.advertise<VescStateStamped>("sensors/core", 1);
  odom_pub_ = nh.advertise<nav_msgs::Odometry>("odom", 1);
  car_status_pub_ = nh.advertise<CarStatusMsg>("car_status", 1);

  ackermann_curvature_sub_ = nh.subscribe(
      "/ackermann_curvature_drive",
      10,
      &VescDriver::ackermannCurvatureCallback,
      this);
  joystick_sub_ = nh.subscribe(
      "joystick", 10, &VescDriver::joystickCallback, this);

  if (kDebug) printf("TIMER START\n");
  // create a 50Hz timer, used for state machine & polling VESC telemetry
  timer_ = nh.createSteadyTimer(ros::WallDuration(kCommandInterval),
                                &VescDriver::timerCallback,
                                this);
  if (kDebug) printf("DONE INIT\n");
}

void VescDriver::checkCommandTimeout() {
  static const double kTimeout = 0.5;
  const double t_now = ros::WallTime::now().toSec();

  ROS_WARN("t_now: %f, t_last_command_: %f, kTimeout: %f", t_now, t_last_command_.load(), kTimeout);

  ROS_WARN("%d", (t_now > t_last_command_ + kTimeout && drive_mode_ == kAutonomousDrive));
  ROS_WARN("%d", t_now > t_last_joystick_ + kTimeout);
  if ((t_now > t_last_command_ + kTimeout && drive_mode_ == kAutonomousDrive)) {
      //t_now > t_last_joystick_ + kTimeout) {
    ROS_WARN("Resetting");
    mux_drive_speed_ = 0;
    mux_steering_angle_ = 0;
  }
}

void VescDriver::joystickCallback(const sensor_msgs::Joy& msg) {
  //static const bool kDebug = false;
  static const float kMaxTurnRate = 0.25;
  static const float kTurboSpeed = 2.0;
  static const float kNormalSpeed = 1.0;
  static const size_t kManualDriveButton = 4;
  static const size_t kAutonomousDriveButton = 5;

  if (msg.buttons.size() < 6) return;
  t_last_joystick_ = ros::WallTime::now().toSec();
  if (msg.buttons[kManualDriveButton] == 1) {
    if(kDebug) printf("Joystick\n");
    drive_mode_ = kJoystickDrive;
  } else if (msg.buttons[kAutonomousDriveButton] == 1) {
    if(kDebug) printf("Autonomous\n");
    drive_mode_ = kAutonomousDrive;
  } else {
    if(kDebug) printf("Stopped\n");
    drive_mode_ = kStoppedDrive;
    mux_drive_speed_ = 0;
    mux_steering_angle_ = 0;
  }
  if (drive_mode_ == kJoystickDrive) {
    if (msg.axes.size() < 5) return;
    const float steer_joystick = -msg.axes[0];
    const float drive_joystick = -msg.axes[4];
    const bool turbo_mode = (msg.axes[2] >= 0.9);
    const float max_speed = (turbo_mode ? kTurboSpeed : kNormalSpeed);
    float speed = drive_joystick * max_speed;
    float steering_angle = steer_joystick * kMaxTurnRate;
    mux_drive_speed_ = speed;
    mux_steering_angle_ = steering_angle;
    if (kDebug) printf("%7.2f %.1f\u00b0\n", speed, math_util::RadToDeg(steering_angle));
  }
}

  /* TODO or TO-THINKABOUT LIST
    - what should we do on startup? send brake or zero command?
    - what to do if the vesc interface gives an error?
    - check version number against know compatable?
    - should we wait until we receive telemetry before sending commands?
    - should we track the last motor command
    - what to do if no motor command received recently?
    - what to do if no servo command received recently?
    - what is the motor safe off state (0 current?)
    - what to do if a command parameter is out of range, ignore?
    - try to predict vesc bounds (from vesc config) and command detect bounds
errors
  */

float Clip(float x, float x_min, float x_max, const char* name) {
  if (x < x_min) {
    fprintf(stderr,
            "Clipping %s value %f to min limit, %f\n",
            name,
            x,
            x_min);
    return x_min;
  }
  if (x > x_max) {
    fprintf(stderr,
            "Clipping %s value %f to max limit, %f\n",
            name,
            x,
            x_max);
    return x_max;
  }
  return x;
}

void VescDriver::sendDriveCommands() {
  //static const bool kDebug = false;
  static const float kMaxAcceleration = 4.0; // m/s^2
  static const float kMaxDeceleration = 6.0; // m/s^2
  static float last_speed_ = 0;

  using math_util::Bound;
  const float max_accel =
    ((last_speed_ > 0.0) ? kMaxAcceleration : kMaxDeceleration);
  const float max_decel =
    ((last_speed_ > 0.0) ? kMaxDeceleration : kMaxAcceleration);
  const float smooth_speed = Bound<float>(
      last_speed_ - kCommandInterval * max_decel,
      last_speed_ + kCommandInterval * max_accel,
      mux_drive_speed_);
  last_speed_ = smooth_speed;
  if (kDebug) {
    printf("%7.2f %7.2f %.1f\u00b0\n",
           mux_drive_speed_, smooth_speed, mux_steering_angle_);
  }
  const float erpm =
      speed_to_erpm_gain_ * smooth_speed + speed_to_erpm_offset_;

  // calc steering angle (servo)
  const float servo = steering_to_servo_gain_ * mux_steering_angle_ +
      steering_to_servo_offset_;

  ROS_INFO("Setting speed to %f", erpm);
  // Set speed command.
  const float erpm_clipped = Clip(erpm, -erpm_speed_limit_, erpm_speed_limit_, "erpm");
  vesc_.setSpeed(erpm_clipped);
  // vesc_.setSpeed(0);

  ROS_INFO("Setting servo to %f", servo);
  // Set servo position command.
  vesc_.setServo(Clip(servo, servo_min_, servo_max_, "servo"));
  last_steering_angle_ = mux_steering_angle_;
}

void VescDriver::timerCallback(const ros::SteadyTimerEvent& event) {
  static const double kMaxInitPeriod = 2.0;
  static const double kTStart = ros::WallTime::now().toSec();

  if (kDebug) printf("TIMER CALLBACK\n");
  checkCommandTimeout();
  // VESC interface should not unexpectedly disconnect, but test for it anyway
  if (!vesc_.isConnected()) {
    fprintf(stderr, "Unexpectedly disconnected from serial port.\n");
    timer_.stop();
    ros::shutdown();
    exit(2);
    return;
  }

  /*
   * Driver state machine, modes:
   *  INITIALIZING - request and wait for vesc version
   *  OPERATING - receiving commands from subscriber topics
   */
  if (driver_mode_ == MODE_INITIALIZING) {
    if (ros::WallTime::now().toSec() > kTStart + kMaxInitPeriod) {
      fprintf(stderr, "FAIL: Timed out while trying to initialize VESC.\n");
      ros::shutdown();
      exit(3);
      return;
    }
    if (kDebug) printf("INITIALIZING\n");
    // request version number, return packet will update the internal version
    // numbers
    vesc_.requestFWVersion();
    if (fw_version_major_ >= 0 && fw_version_minor_ >= 0) {
      printf("Connected to VESC with firmware version %d.%d\n",
             fw_version_major_, fw_version_minor_);
      driver_mode_ = MODE_OPERATING;
    }
  } else if (driver_mode_ == MODE_OPERATING) {
    sendDriveCommands();
    if (kDebug) printf("OPERATING\n");
    // poll for vesc state (telemetry)
    vesc_.requestState();
  } else {
    if (kDebug) printf("FAIL: UNKNOWN STATE!\n");
    // unknown mode, how did that happen?
    assert("unknown driver mode");
  }
}

void VescDriver::updateOdometry(float rpm, float steering_angle) {
  // TODO: calculate speed based on tachometer as opposed to rpm

  // Calcuate linear velocity
  float lin_vel = (rpm - speed_to_erpm_offset_) / speed_to_erpm_gain_;
  // Clamp velocity to zero for minuscule values - a VESC drift issue.
  if (fabs(lin_vel) < 0.01) {
    lin_vel = 0.0;
  }
  // Calculate angular velocity
  float turn_radius = 0;
  float rot_vel = 0;
  if (steering_angle != 0) {
    turn_radius = wheelbase_ / tan(steering_angle);
    rot_vel = lin_vel / turn_radius;
  }

  static float position_x = 0;
  static float position_y = 0;
  static float orientation = 0; // theta
  static double last_frame_time = ros::Time::now().toSec();
  ros::Time current_frame_time_ros = ros::Time::now();
  double current_frame_time = current_frame_time_ros.toSec();

  // Update the estimated pose
  double del_t = current_frame_time - last_frame_time;

  // Enforce monotonically increasing time stamps
  if (del_t >= 0) {
    float del_x = lin_vel * del_t * cos(orientation);
    float del_y = lin_vel * del_t * sin(orientation);
    float del_theta = rot_vel * del_t;

    position_x = position_x + del_x;
    position_y = position_y + del_y;
    orientation = math_util::AngleMod(orientation + del_theta);

    // Create an odometry message
    odom_msg_.twist.twist.linear.x = lin_vel;
    odom_msg_.twist.twist.angular.z = rot_vel;
    odom_msg_.pose.pose.position.x = position_x;
    odom_msg_.pose.pose.position.y = position_y;
    odom_msg_.pose.pose.orientation.w = cos(0.5 * orientation);
    odom_msg_.pose.pose.orientation.z = sin(0.5 * orientation);
    odom_pub_.publish(odom_msg_);
  } else {
    printf("Odometry messages received out of order.\n") ;
  }
  last_frame_time = current_frame_time;
}

void VescDriver::vescPacketCallback(const boost::shared_ptr<VescPacket const>&
packet)
{
  if (packet->name() == "Values") {
    boost::shared_ptr<VescPacketValues const> values =
      boost::dynamic_pointer_cast<VescPacketValues const>(packet);

    state_msg_.header.stamp = ros::Time::now();
    state_msg_.state.voltage_input = values->v_in();
    state_msg_.state.temperature_pcb = values->temp_pcb();
    state_msg_.state.current_motor = values->current_motor();
    state_msg_.state.current_input = values->current_in();
    state_msg_.state.speed = values->rpm();
    state_msg_.state.duty_cycle = values->duty_now();
    state_msg_.state.charge_drawn = values->amp_hours();
    state_msg_.state.charge_regen = values->amp_hours_charged();
    state_msg_.state.energy_drawn = values->watt_hours();
    state_msg_.state.energy_regen = values->watt_hours_charged();
    state_msg_.state.displacement = values->tachometer();
    state_msg_.state.distance_traveled = values->tachometer_abs();
    state_msg_.state.fault_code = values->fault_code();
    state_pub_.publish(state_msg_);

    car_status_msg_.header.stamp = ros::Time::now();
    car_status_msg_.battery_voltage = values->v_in();
    car_status_msg_.status = static_cast<uint8_t>(drive_mode_);
    car_status_pub_.publish(car_status_msg_);

    updateOdometry(values->rpm(), last_steering_angle_);

  }
  else if (packet->name() == "FWVersion") {
    boost::shared_ptr<VescPacketFWVersion const> fw_version =
      boost::dynamic_pointer_cast<VescPacketFWVersion const>(packet);
    // todo: might need lock here
    fw_version_major_ = fw_version->fwMajor();
    fw_version_minor_ = fw_version->fwMinor();
  }
}

void VescDriver::vescErrorCallback(const std::string& error) {
  fprintf(stderr, "VESC Error: %s\n", error.c_str());
}

float VescDriver::CalculateSteeringAngle(float lin_vel, float rot_vel) {
  float steering_angle = 0.0;
  if (rot_vel == 0) {
    return steering_angle;
  }

  float turn_radius = lin_vel / rot_vel;
  steering_angle = std::atan(wheelbase_ / turn_radius);
  return steering_angle;
}

void VescDriver::ackermannCurvatureCallback(const f1tenth_course::AckermannCurvatureDriveMsg& cmd) {
  ROS_INFO("Ackermann drive command received, vel: %f, curve: %f", cmd.velocity, cmd.curvature);
  t_last_command_ = ros::WallTime::now().toSec();
  drive_mode_ = kAutonomousDrive;
  if (drive_mode_ == kAutonomousDrive) {
    mux_drive_speed_ = cmd.velocity;
    const float rot_vel = cmd.velocity * cmd.curvature;
    mux_steering_angle_ = CalculateSteeringAngle(mux_drive_speed_, rot_vel);
  }
}

} // namespace vesc_driver
