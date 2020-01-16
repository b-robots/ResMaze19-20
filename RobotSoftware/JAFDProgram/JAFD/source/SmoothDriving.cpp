/*
This part of the Library is responsible for driving smoothly.
*/

#if defined(ARDUINO) && ARDUINO >= 100
#include "arduino.h"
#else
#include "WProgram.h"
#endif

#include <new>

#include "../header/SmoothDriving.h"
#include "../header/MotorControl.h"
#include "../header/SensorFusion.h"
#include "../header/Math.h"
#include "../header/PIDController.h"

namespace JAFD
{
	namespace SmoothDriving
	{
		namespace
		{
			// Copy of current task
			union _TaskCopies
			{
				Accelerate accelerate;
				DriveStraight straight;
				Stop stop;
				Rotate rotate;

				_TaskCopies() : stop() {};
			} _taskCopies;			

			ITask* _currentTask = &_taskCopies.stop;	// Current task
			
			PIDController _forwardVelPID(JAFDSettings::Controller::SmoothDriving::forwardVelPidSettings);	// PID controller for forward velocity
			PIDController _angularVelPID(JAFDSettings::Controller::SmoothDriving::angularVelPidSettings);	// PID controller for angular velocity
		}

		// Accelerate class - begin 

		Accelerate::Accelerate(int16_t endSpeeds, float distance) : ITask(), _endSpeeds(endSpeeds), _distance(distance), _targetDir(1.0f, 0.0f){}

		ReturnCode Accelerate::startTask(RobotState startState)
		{
			_finished = false;
			_targetDir = Vec2f(cosf(startState.rotation.x), sinf(startState.rotation.x));
			_startPos = (Vec2f)(startState.position);
			_startSpeeds = startState.forwardVel;

			if (!((_startSpeeds >= 0 && _endSpeeds >= 0 && _distance >= 0) || (_startSpeeds <= 0 && _endSpeeds <= 0 && _distance <= 0)) || _endSpeeds == _startSpeeds) return ReturnCode::error;

			_totalTime = 2 * _distance / static_cast<float>(_endSpeeds + _startSpeeds);

			_endState.wheelSpeeds = FloatWheelSpeeds{ _endSpeeds, _endSpeeds };
			_endState.forwardVel = static_cast<float>(_endSpeeds);
			_endState.position = startState.position + (Vec3f)(_targetDir * _distance);
			_endState.angularVel = Vec3f(0.0f, 0.0f, 0.0f);
			_endState.rotation = startState.rotation;

			return ReturnCode::ok;
		}

		// Update speeds for both wheels
		WheelSpeeds Accelerate::updateSpeeds(const uint8_t freq)
		{
			static Vec2f currentPosition;		// Current position of robot
			static float currentHeading;		// Current heading of robot;
			static Vec2f posRelToStart;			// Position relative to start
			static float drivenDistance;		// Distance to startpoint (with correct sign for direction)
			static float radiant;				// Needed as temporary value for acceleration/decceleration
			static float calculatedTime;		// Calculated time based on driven distance
			static float desiredSpeed;			// Desired linear velocity (calculated by acceleration/decceleration calculation + PID controller)
			static float desAngularVel;			// Desired angular velocity (calculated by pure pursuit algorithm + PID controller)
			static float lookAheadDistance;		// Lookahead distance for pure pursuit algorithm
			static Vec2f goalPointGlobal;		// Goal-Point in global coordinate space for pure pursuit algorithm
			static Vec2f goalPointRobot;		// Goal-Point in robot coordinate space for pure pursuit algorithm
			static float desCurvature;			// Desired curvature (calculated by pure pursuit algorithm)
			static float correctedForwardVel;	// Corrected forward velocity
			static float correctedAngularVel;	// Corrected angular velocity
			static WheelSpeeds output;			// Speed output for both wheels

			currentPosition = (Vec2f)(SensorFusion::getRobotState().position);
			currentHeading = SensorFusion::getRobotState().rotation.x;

			// Calculate driven distance
			posRelToStart = currentPosition - _startPos;
			drivenDistance = posRelToStart.length();

			// Check if I am there
			if (drivenDistance >= abs(_distance))
			{
				_finished = true;

				if (_endSpeeds == 0)
				{
					_forwardVelPID.reset();
					_angularVelPID.reset();

					return WheelSpeeds{ 0, 0 };
				}
			}

			drivenDistance *= sgn(_distance);

			// If finished, drive with end speeds
			if (!_finished)
			{
				// Accelerate / deccelerate - v = v_1 + (t / t_ges) * (v_2 - v_1); s = _errorIntegral(v * dt) = ((v_2 - v_1) * t^2) / (2 * t_ges) + v_1 * t => radiant = t_ges * v_1^2 - 2 * s * (v_1 + v_2); t = t_ges * (v_1 - sqrt(radiant)) / (v_2 - v_1); t_ges = s * 2 / (v_2 - v_1)
				radiant = static_cast<float>(_startSpeeds) * static_cast<float>(_startSpeeds) + 2.0f * drivenDistance * static_cast<float>(_endSpeeds - _startSpeeds) / _totalTime;
				calculatedTime = _totalTime * (static_cast<float>(_startSpeeds) - sqrtf(abs(radiant)) * sgn(_startSpeeds + _endSpeeds)) / static_cast<float>(_startSpeeds - _endSpeeds);
				calculatedTime = abs(calculatedTime);
				desiredSpeed = static_cast<float>(_startSpeeds) + (calculatedTime / _totalTime) * static_cast<float>(_endSpeeds - _startSpeeds);
			}
			else
			{
				desiredSpeed = _endSpeeds;
			}

			// A variation of pure pursuits controller where the goal point is a lookahead distance on the path away (not a lookahead distance from the robot).
			// Furthermore, the lookahead distance is dynamically adapted to the speed
			// Calculate goal point
			lookAheadDistance = JAFDSettings::Controller::PurePursuit::lookAheadGain * desiredSpeed;

			if (lookAheadDistance < JAFDSettings::Controller::PurePursuit::minLookAheadDist && lookAheadDistance > -JAFDSettings::Controller::PurePursuit::minLookAheadDist) lookAheadDistance = JAFDSettings::Controller::PurePursuit::minLookAheadDist * sgn(desiredSpeed);

			goalPointGlobal = _startPos + _targetDir * (drivenDistance + lookAheadDistance);

			// Transform goal point to robot coordinates
			goalPointRobot.x = (goalPointGlobal.x - currentPosition.x)  * cosf(currentHeading) + (goalPointGlobal.y - currentPosition.y) * sinf(currentHeading);
			goalPointRobot.y = -(goalPointGlobal.x - currentPosition.x) * sinf(currentHeading) + (goalPointGlobal.y - currentPosition.y) * cosf(currentHeading);

			// Calculate curvature and angular velocity
			desCurvature = 2.0f * goalPointRobot.y / (goalPointRobot.x * goalPointRobot.x + goalPointRobot.y * goalPointRobot.y);
			
			if (desCurvature > JAFDSettings::Controller::PurePursuit::maxCurvature) desCurvature = JAFDSettings::Controller::PurePursuit::maxCurvature;
			else if (desCurvature < -JAFDSettings::Controller::PurePursuit::maxCurvature) desCurvature = -JAFDSettings::Controller::PurePursuit::maxCurvature;

			desAngularVel = desiredSpeed * desCurvature;

			// Kind of PID - controller
			correctedForwardVel = desiredSpeed * 0.8f + _forwardVelPID.process(desiredSpeed, SensorFusion::getRobotState().forwardVel, 1.0f / freq);
			correctedAngularVel = desAngularVel;//_angularVelPID.process(desAngularVel, SensorFusion::getRobotState().angularVel, 1.0f / freq);

			// Compute wheel speeds - v = (v_r + v_l) / 2; w = (v_r - v_l) / wheelDistance => v_l = v - w * wheelDistance / 2; v_r = v + w * wheelDistance / 2
			output = WheelSpeeds{ correctedForwardVel - JAFDSettings::Mechanics::wheelDistance * correctedAngularVel / 2.0f, correctedForwardVel + JAFDSettings::Mechanics::wheelDistance * correctedAngularVel / 2.0f };

			// Correct speed if it is too low 
			if (output.left < JAFDSettings::MotorControl::minSpeed && output.left > -JAFDSettings::MotorControl::minSpeed) output.left = JAFDSettings::MotorControl::minSpeed * sgn(_distance);
			if (output.right < JAFDSettings::MotorControl::minSpeed && output.right > -JAFDSettings::MotorControl::minSpeed) output.right = JAFDSettings::MotorControl::minSpeed * sgn(_distance);
			
			return output;
		}

		// Accelerate class - end

		// DriveStraight class - begin

		DriveStraight::DriveStraight(float distance) : ITask(), _distance(distance), _targetDir(1.0f, 0.0f) {}

		ReturnCode DriveStraight::startTask(RobotState startState)
		{
			_finished = false;
			_targetDir = Vec2f(cosf(startState.rotation.x), sinf(startState.rotation.x));
			_startPos = (Vec2f)(startState.position);
			_speeds = startState.forwardVel;

			if (sgn(_speeds) != sgn(_distance)) return ReturnCode::error;

			if (_speeds <= 0)
			{
				_targetDir *= -1;
			}

			_endState.wheelSpeeds = FloatWheelSpeeds{ _speeds, _speeds };
			_endState.forwardVel = static_cast<float>(_speeds);
			_endState.position = startState.position + (Vec3f)(_targetDir * abs(_distance));
			_endState.angularVel = Vec3f(0.0f, 0.0f, 0.0f);
			_endState.rotation = startState.rotation;

			return ReturnCode::ok;
		}

		// Update speeds for both wheels
		WheelSpeeds DriveStraight::updateSpeeds(const uint8_t freq)
		{
			static Vec2f currentPosition;		// Current position of robot
			static float currentHeading;		// Current heading of robot;
			static Vec2f posRelToStart;			// Position relative to start
			static float absDrivenDist;			// Absolute distance to startpoint
			static float radiant;				// Needed as temporary value for acceleration/decceleration
			static float calculatedTime;		// Calculated time based on driven distance
			static float desAngularVel;			// Desired angular velocity (calculated by pure pursuit algorithm + PID controller)
			static float lookAheadDistance;		// Lookahead distance for pure pursuit algorithm
			static Vec2f goalPointGlobal;		// Goal-Point in global coordinate space for pure pursuit algorithm
			static Vec2f goalPointRobot;		// Goal-Point in robot coordinate space for pure pursuit algorithm
			static float desCurvature;			// Desired curvature (calculated by pure pursuit algorithm)
			static WheelSpeeds output;			// Speed output for both wheels
			static float correctedForwardVel;	// Corrected forward velocity
			static float correctedAngularVel;	// Corrected angular velocity

			currentPosition = (Vec2f)(SensorFusion::getRobotState().position);
			currentHeading = SensorFusion::getRobotState().rotation.x;

			// Calculate driven distance
			posRelToStart = currentPosition - _startPos;
			absDrivenDist = posRelToStart.length();

			// Check if I am there
			if (abs(absDrivenDist) >= abs(_distance))
			{
				_finished = true;
			}

			// A variation of pure pursuits controller where the goal point is a lookahead distance on the path away (not a lookahead distance from the robot).
			// Furthermore, the lookahead distance is dynamically adapted to the speed
			// Calculate goal point
			lookAheadDistance = JAFDSettings::Controller::PurePursuit::lookAheadGain * _speeds;
			lookAheadDistance = abs(lookAheadDistance);

			if (lookAheadDistance < JAFDSettings::Controller::PurePursuit::minLookAheadDist) lookAheadDistance = JAFDSettings::Controller::PurePursuit::minLookAheadDist;

			goalPointGlobal = _startPos + _targetDir * (absDrivenDist + lookAheadDistance);

			// Transform goal point to robot coordinates
			goalPointRobot.x = (goalPointGlobal.x - currentPosition.x)  * cosf(currentHeading) + (goalPointGlobal.y - currentPosition.y) * sinf(currentHeading);
			goalPointRobot.y = -(goalPointGlobal.x - currentPosition.x) * sinf(currentHeading) + (goalPointGlobal.y - currentPosition.y) * cosf(currentHeading);

			// Calculate curvature and angular velocity
			desCurvature = 2.0f * goalPointRobot.y / (goalPointRobot.x * goalPointRobot.x + goalPointRobot.y * goalPointRobot.y);

			if (desCurvature > JAFDSettings::Controller::PurePursuit::maxCurvature) desCurvature = JAFDSettings::Controller::PurePursuit::maxCurvature;
			else if (desCurvature < -JAFDSettings::Controller::PurePursuit::maxCurvature) desCurvature = -JAFDSettings::Controller::PurePursuit::maxCurvature;

			desAngularVel = _speeds * desCurvature;

			// Kind of PID - controller
			correctedForwardVel = _speeds * 0.8f + _forwardVelPID.process(_speeds, SensorFusion::getRobotState().forwardVel, 1.0f / freq);
			correctedAngularVel = desAngularVel;//_angularVelPID.process(desAngularVel, SensorFusion::getRobotState().angularVel, 1.0f / freq);

			// Compute wheel speeds - v = (v_r + v_l) / 2; w = (v_r - v_l) / wheelDistance => v_l = v - w * wheelDistance / 2; v_r = v + w * wheelDistance / 2
			output = WheelSpeeds{ correctedForwardVel - JAFDSettings::Mechanics::wheelDistance * correctedAngularVel / 2.0f, correctedForwardVel + JAFDSettings::Mechanics::wheelDistance * correctedAngularVel / 2.0f };

			// Correct speed if it is too low 
			if (output.left < JAFDSettings::MotorControl::minSpeed && output.left > -JAFDSettings::MotorControl::minSpeed) output.left = JAFDSettings::MotorControl::minSpeed * sgn(_distance);
			if (output.right < JAFDSettings::MotorControl::minSpeed && output.right > -JAFDSettings::MotorControl::minSpeed) output.right = JAFDSettings::MotorControl::minSpeed * sgn(_distance);

			return output;
		}

		// DriveStraight class - end

		// Stop class - begin

		ReturnCode Stop::startTask(RobotState startState)
		{
			_finished = false;
			_endState.wheelSpeeds = FloatWheelSpeeds{ 0.0f, 0.0f };
			_endState.forwardVel = static_cast<float>(0.0f);
			_endState.position = startState.position;
			_endState.angularVel = Vec3f(0.0f, 0.0f, 0.0f);
			_endState.rotation = startState.rotation;

			_forwardVelPID.reset();
			_angularVelPID.reset();

			return ReturnCode::ok;
		}

		WheelSpeeds Stop::updateSpeeds(const uint8_t freq)
		{
			_finished = true;

			return WheelSpeeds{ 0, 0 };
		}

		// Stop class - end

		// Rotate class - begin

		Rotate::Rotate(int16_t maxAngularVel, float angle) : ITask(), _maxAngularVel(maxAngularVel), _angle(angle) {}

		ReturnCode Rotate::startTask(RobotState startState)
		{
			_finished = false;
			_startAngle = startState.rotation.x;

			if (abs(startState.wheelSpeeds.left) < JAFDSettings::MotorControl::minSpeed || abs(startState.wheelSpeeds.right) < JAFDSettings::MotorControl::minSpeed) return ReturnCode::error;

			_totalTime = _angle / _maxAngularVel;

			if (_totalTime < 0.0f) return ReturnCode::error;

			_endState.wheelSpeeds = FloatWheelSpeeds{ 0.0f, 0.0f };
			_endState.forwardVel = static_cast<float>(0.0f);
			_endState.position = startState.position;
			_endState.angularVel = Vec3f(0.0f, 0.0f, 0.0f);
			_endState.rotation = startState.rotation + Vec3f(_angle, 0.0f, 0.0f);

			_forwardVelPID.reset();
			_angularVelPID.reset();

			return ReturnCode::ok;
		}

		// Update speeds for both wheels
		WheelSpeeds Rotate::updateSpeeds(const uint8_t freq)
		{
			static float rotatedAngle;			// Rotated angle since start
			static float desAngularVel;			// Desired angular velocity
			static float correctedAngularVel;	// By PID Controller corrected angular velocity
			static bool accelerate;				// Still accelerate? 
			static WheelSpeeds output;			// Output for both wheels

			// Calculate rotated angle
			rotatedAngle = SensorFusion::getRobotState().rotation.x - _startAngle;

			// Check if I am there
			if (abs(rotatedAngle) >= abs(_angle))
			{
				_finished = true;

				_forwardVelPID.reset();
				_angularVelPID.reset();

				return WheelSpeeds{ 0, 0 };
			}

			// Accelerate / deccelerate
			if (accelerate)
			{
				// w(t) = w_max * 2 * t / t_ges => a(t) = w_max * t^2 / t_ges => t(a) = sqrt(a * t_ges / w_max); w(a) = w_max * 2 * sqrt(a * t_ges / w_max) / t_ges = sqrt(4 * a * w_max / t_ges)
				desAngularVel = sqrtf(4.0f * rotatedAngle * _maxAngularVel / _totalTime) * sgn(_maxAngularVel);

				if (abs(desAngularVel) >= abs(_maxAngularVel)) accelerate = false;
			}
			else
			{
				// w(t) = w_max * 2 * (t_ges - t) / t_ges => a(t) = -(w_max * (2 * t - 5 * t_ges) * (2 * t - t_ges)) / (8 * t_ges) => t(a) = sqrt(a * t_ges / w_max); w(a) = w_max * 2 * sqrt(a * t_ges / w_max) / t_ges = sqrt(4 * a * w_max / t_ges)
				desAngularVel = sqrtf(4.0f * rotatedAngle * _maxAngularVel / _totalTime) * sgn(_maxAngularVel);
			}

			// Kind of PID - controller
			correctedAngularVel = desAngularVel * 0.8 + _angularVelPID.process(desAngularVel, SensorFusion::getRobotState().angularVel.x, 1.0f / freq);

			// Compute wheel speeds -- w = (v_r - v_l) / wheelDistance; v_l = -v_r; => v_l = -w * wheelDistance / 2; v_r = w * wheelDistance / 2
			output = WheelSpeeds{ -JAFDSettings::Mechanics::wheelDistance * correctedAngularVel / 2.0f, JAFDSettings::Mechanics::wheelDistance * correctedAngularVel / 2.0f };

			// Correct speed if it is too low
			if (output.right < JAFDSettings::MotorControl::minSpeed && output.right > -JAFDSettings::MotorControl::minSpeed)
			{
				output.right = JAFDSettings::MotorControl::minSpeed * sgn(_angle);
				output.left = -output.right;
			}

			return output;
		}

		// Rotate class - end

		// Update speeds for both wheels
		void updateSpeeds(const uint8_t freq)
		{
			MotorControl::setSpeeds(_currentTask->updateSpeeds(freq));
		}

		// Set new Accelerate task (use last end state to start)
		template<>
		ReturnCode setNewTask<NewStateType::lastEndState>(const Accelerate& newTask, const bool forceOverride)
		{
			static RobotState endState;
			static ReturnCode returnCode;
			static Accelerate temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				endState = static_cast<RobotState>(_currentTask->_endState);

				temp = newTask;
				returnCode = temp.startTask(endState);

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.accelerate)) Accelerate(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new Accelerate task (use current state to start)
		template<>
		ReturnCode setNewTask<NewStateType::currentState>(const Accelerate& newTask, const bool forceOverride)
		{
			static ReturnCode returnCode;
			static Accelerate temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				temp = newTask;
				returnCode = temp.startTask(SensorFusion::getRobotState());

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.accelerate)) Accelerate(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new Accelerate task (use specified state to start)
		ReturnCode setNewTask(const Accelerate& newTask, RobotState startState, const bool forceOverride)
		{
			static ReturnCode returnCode;
			static Accelerate temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				temp = newTask;
				returnCode = temp.startTask(startState);

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.accelerate)) Accelerate(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new DriveStraight task (use last end state to start)
		template<>
		ReturnCode setNewTask<NewStateType::lastEndState>(const DriveStraight& newTask, const bool forceOverride)
		{
			static RobotState endState;
			static ReturnCode returnCode;
			static DriveStraight temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				endState = static_cast<RobotState>(_currentTask->_endState);

				temp = newTask;
				returnCode = temp.startTask(endState);

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.straight)) DriveStraight(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new DriveStraight task (use current state to start)
		template<>
		ReturnCode setNewTask<NewStateType::currentState>(const DriveStraight& newTask, const bool forceOverride)
		{
			static ReturnCode returnCode;
			static DriveStraight temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				temp = newTask;
				returnCode = temp.startTask(SensorFusion::getRobotState());

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.straight)) DriveStraight(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new DriveStraight task (use specified state to start)
		ReturnCode setNewTask(const DriveStraight& newTask, RobotState startState, const bool forceOverride)
		{
			static ReturnCode returnCode;
			static DriveStraight temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				temp = newTask;
				returnCode = temp.startTask(startState);

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.straight)) DriveStraight(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new Stop task (use last end state to start)
		template<>
		ReturnCode setNewTask<NewStateType::lastEndState>(const Stop& newTask, const bool forceOverride)
		{
			static RobotState endState;
			static ReturnCode returnCode;
			static Stop temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				endState = static_cast<RobotState>(_currentTask->_endState);

				temp = newTask;
				returnCode = temp.startTask(endState);

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.stop)) Stop(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new Stop task (use current state to start)
		template<>
		ReturnCode setNewTask<NewStateType::currentState>(const Stop& newTask, const bool forceOverride)
		{
			static ReturnCode returnCode;
			static Stop temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				temp = newTask;
				returnCode = temp.startTask(SensorFusion::getRobotState());

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.stop)) Stop(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Set new DriveStraight task (use specified state to start)
		ReturnCode setNewTask(const Stop& newTask, RobotState startState, const bool forceOverride)
		{
			static ReturnCode returnCode;
			static Stop temp;

			returnCode = ReturnCode::ok;

			__disable_irq();

			if (_currentTask->_finished || forceOverride)
			{
				temp = newTask;
				returnCode = temp.startTask(startState);

				if (returnCode == ReturnCode::ok)
				{
					_currentTask = new (&(_taskCopies.stop)) Stop(temp);
				}
			}

			__enable_irq();
			return returnCode;
		}

		// Is the current task finished?
		bool isTaskFinished()
		{
			return _currentTask->_finished;
		}
	}
}