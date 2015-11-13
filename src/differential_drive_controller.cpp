//
// Created by paul on 30.04.15.
//

#include <vehicle_controller/differential_drive_controller.h>

DifferentialDriveController::~DifferentialDriveController()
{
    if(dr_default_server_)
        delete dr_default_server_;
    if(dr_argo_server_)
        delete dr_argo_server_;
}

void DifferentialDriveController::configure(ros::NodeHandle& params, MotionParameters* mp)
{
    mp_ = mp;

    ros::NodeHandle nh;
    cmd_vel_raw_pub_ = nh.advertise<geometry_msgs::Twist>("cmd_vel_raw", 1);
    pdout_pub_ = nh.advertise<monstertruck_msgs::Pdout>("pdout", 1);

    params.getParam("max_controller_speed", mp_->max_controller_speed_);
    params.getParam("max_unlimited_speed", mp_->max_unlimited_speed_);
    params.getParam("max_controller_angular_rate", mp_->max_controller_angular_rate_);
    params.getParam("max_unlimited_angular_rate", mp_->max_unlimited_angular_rate_);

    KP_ANGLE_ = 2.0;
    KD_ANGLE_ = 0.5;
    KP_POSITION_ = 0.5;
    KD_POSITION_ = 0.0;
    SPEED_REDUCTION_GAIN_ = 2.0;

    if(mp_->pd_params == "PdParamsArgo")
    {
        dr_argo_server_ = new dynamic_reconfigure::Server<vehicle_controller::PdParamsArgoConfig>;
        dr_argo_server_->setCallback(boost::bind(&DifferentialDriveController::pdParamCallback<vehicle_controller::PdParamsArgoConfig>, this, _1, _2));
    }
    else
    {
        dr_default_server_ = new dynamic_reconfigure::Server<vehicle_controller::PdParamsConfig>;
        dr_default_server_->setCallback(boost::bind(&DifferentialDriveController::pdParamCallback<vehicle_controller::PdParamsConfig>, this, _1, _2));
    }
}

void DifferentialDriveController::executeUnlimitedTwist(const geometry_msgs::Twist& inc_twist)
{
    twist = inc_twist;
    twist.angular.z = std::max(-mp_->max_unlimited_angular_rate_,
                               std::min(mp_->max_unlimited_angular_rate_, twist.angular.z));
    twist.linear.x  = std::max(-mp_->max_unlimited_speed_,
                               std::min(mp_->max_unlimited_speed_, twist.linear.x));
    cmd_vel_raw_pub_.publish(twist);
}

void DifferentialDriveController::executeTwist(const geometry_msgs::Twist& inc_twist)
{
    twist = inc_twist;
    this->limitTwist(twist, mp_->max_controller_speed_, mp_->max_controller_angular_rate_);
    cmd_vel_raw_pub_.publish(twist);
}

/**
 * @brief DifferentialDriveController::executePDControlledMotionCommand
 * @param e_angle the angular error which is assumed to lie inside [-pi,pi]
 * @param e_position the position error
 * @param dt time difference between two control loop iterates
 */
void DifferentialDriveController::executePDControlledMotionCommand(double e_angle, double e_position, double dt, double cmded_speed)
{
    static double previous_e_angle = e_angle;
    static double previous_e_position = e_position;

    if(mp_->isYSymmetric())
    {
        if(e_angle > M_PI_2)
            e_angle = e_angle - M_PI;
        if(e_angle < -M_PI_2)
            e_angle = M_PI + e_angle;
    }

    double de_angle_dt    = (e_angle - previous_e_angle) / dt; // causes discontinuity @ orientation_error vs relative_angle switch
    double de_position_dt = (e_position - previous_e_position) / dt;

    double speed   = KP_POSITION_ * e_position + KD_POSITION_ * de_position_dt;
    double z_angular_rate = KP_ANGLE_ * e_angle + KD_ANGLE_ * de_angle_dt;

    if(fabs(speed) > fabs(cmded_speed))
        speed = (speed < 0 ? -1.0 : 1.0) * fabs(mp_->commanded_speed);

    twist.linear.x = speed;
    twist.angular.z = z_angular_rate;
    this->limitTwist(twist, mp_->max_controller_speed_, mp_->max_controller_angular_rate_);
    cmd_vel_raw_pub_.publish(twist);

    monstertruck_msgs::Pdout pdout;
    pdout.header.frame_id = "world";
    pdout.header.stamp = ros::Time::now();
    pdout.dt = dt;
    pdout.e_position = e_position;
    pdout.e_angle = e_angle;
    pdout.de_position_dt = de_position_dt;
    pdout.de_angle_dt = de_angle_dt;
    pdout.speed = speed;
    pdout.z_twist = z_angular_rate;
    pdout.z_twist_real = twist.angular.z;
    pdout.z_twist_deg = z_angular_rate / M_PI * 180;
    pdout.speed_real = twist.linear.x;
    pdout.z_twist_deg_real = twist.angular.z / M_PI * 180;
    pdout_pub_.publish(pdout);

    previous_e_angle = e_angle;
    previous_e_position = e_position;
}

void DifferentialDriveController::executeMotionCommand(double carrot_relative_angle,
    double carrot_orientation_error, double carrot_distance, double speed,
    double signed_carrot_distance_2_robot, double dt)
{
//    double e_angle = speed < 0 ? carrot_orientation_error : carrot_relative_angle;
//    if(signed_carrot_distance_2_robot < 0 && fabs(e_angle) > M_PI_4)
//        e_angle = carrot_relative_angle;
    double e_angle = carrot_relative_angle;
    if(e_angle > M_PI + 1e-2 || e_angle < -M_PI - 1e-2)
    {
        ROS_WARN("[vehicle_controller] [differential_drive_controller] Invalid angle was given.");
    }
    if(speed == 0.0)
    {
        ROS_INFO("[vehicle_controller] [differential_drive_controller] Commanded speed is 0");
        speed = 0.0;
    }
    executePDControlledMotionCommand(e_angle, signed_carrot_distance_2_robot, dt, speed);
    // executeMotionCommand(carrot_relative_angle, carrot_orientation_error, carrot_distance, speed);
}

void DifferentialDriveController::executeMotionCommand(double carrot_relative_angle, double carrot_orientation_error, double carrot_distance, double speed)
{
    float sign = speed < 0.0 ? -1.0 : 1.0;

    twist.linear.x = speed;

    if (sign < 0)
        twist.angular.z = carrot_orientation_error / carrot_distance * 1.5 * 0.25;
    else
        twist.angular.z = carrot_relative_angle / carrot_distance * 1.5;

    this->limitTwist(twist, mp_->max_controller_speed_, mp_->max_controller_angular_rate_);

    cmd_vel_raw_pub_.publish(twist);
}

void DifferentialDriveController::stop()
{
    twist.angular.z = 0.0;
    twist.linear.x = 0.0;
    cmd_vel_raw_pub_.publish(twist);
}

void DifferentialDriveController::limitTwist(geometry_msgs::Twist& twist, double max_speed, double max_angular_rate)
{
    double speed = twist.linear.x;
    double angular_rate = twist.angular.z;

    speed        = std::max(-mp_->max_unlimited_speed_, std::min(mp_->max_unlimited_speed_, speed));
    angular_rate = std::max(-mp_->max_unlimited_angular_rate_, std::min(mp_->max_unlimited_angular_rate_, angular_rate));

    double m = -mp_->max_controller_speed_ / mp_->max_controller_angular_rate_;
    double t = mp_->max_controller_speed_;
    double speedAbsUL = std::min(std::max(0.0, m * std::abs(angular_rate) * SPEED_REDUCTION_GAIN_ + t), max_speed);

    twist.linear.x = std::max(-speedAbsUL, std::min(speed, speedAbsUL));
    twist.angular.z = std::max(-max_angular_rate, std::min(max_angular_rate, angular_rate));
}
