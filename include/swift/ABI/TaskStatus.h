//===--- TaskStatusRecord.h - Structures to track task status --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Swift ABI describing "status records", the mechanism by which
// tasks track dynamic information about their child tasks, custom
// cancellation hooks, and other information which may need to be exposed
// asynchronously outside of the task.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_TASKSTATUS_H
#define SWIFT_ABI_TASKSTATUS_H

#include "swift/ABI/MetadataValues.h"
#include "swift/ABI/Task.h"

namespace swift {

/// The abstract base class for all status records.
///
/// TaskStatusRecords are typically allocated on the stack (possibly
/// in the task context), partially initialized, and then atomically
/// added to the task with `swift_task_addTaskStatusRecord`.  While
/// registered with the task, a status record should only be
/// modified in ways that respect the possibility of asynchronous
/// access by a cancelling thread.  In particular, the chain of
/// status records must not be disturbed.  When the task leaves
/// the scope that requires the status record, the record can
/// be unregistered from the task with `removeStatusRecord`,
/// at which point the memory can be returned to the system.
class TaskStatusRecord {
public:
  TaskStatusRecordFlags Flags;
  TaskStatusRecord *Parent;

  TaskStatusRecord(TaskStatusRecordKind kind,
                   TaskStatusRecord *parent = nullptr)
      : Flags(kind) {
    resetParent(parent);
  }

  TaskStatusRecord(const TaskStatusRecord &) = delete;
  TaskStatusRecord &operator=(const TaskStatusRecord &) = delete;

  TaskStatusRecordKind getKind() const { return Flags.getKind(); }

  TaskStatusRecord *getParent() const { return Parent; }

  /// Change the parent of this unregistered status record to the
  /// given record.
  ///
  /// This should be used when the record has been previously initialized
  /// without knowing what the true parent is.  If we decide to cache
  /// important information (e.g. the earliest timeout) in the innermost
  /// status record, this is the method that should fill that in
  /// from the parent.
  void resetParent(TaskStatusRecord *newParent) {
    Parent = newParent;
    // TODO: cache
  }

  /// Splice a record out of the status-record chain.
  ///
  /// Unlike resetParent, this assumes that it's just removing one or
  /// more records from the chain and that there's no need to do any
  /// extra cache manipulation.
  void spliceParent(TaskStatusRecord *newParent) { Parent = newParent; }
};

/// A deadline for the task.  If this is reached, the task will be
/// automatically cancelled.  The deadline can also be queried and used
/// in other ways.
struct TaskDeadline {
  // FIXME: I don't really know what this should look like right now.
  // It's probably target-specific.
  uint64_t Value;

  bool operator==(const TaskDeadline &other) const {
    return Value == other.Value;
  }
  bool operator<(const TaskDeadline &other) const {
    return Value < other.Value;
  }
};

/// A status record which states that there's an active deadline
/// within the task.
class DeadlineStatusRecord : public TaskStatusRecord {
  TaskDeadline Deadline;

public:
  DeadlineStatusRecord(TaskDeadline deadline)
      : TaskStatusRecord(TaskStatusRecordKind::Deadline), Deadline(deadline) {}

  TaskDeadline getDeadline() const { return Deadline; }

  static bool classof(const TaskStatusRecord *record) {
    return record->getKind() == TaskStatusRecordKind::Deadline;
  }
};

/// A status record which states that a task has one or
/// more active child tasks.
class ChildTaskStatusRecord : public TaskStatusRecord {
  AsyncTask *FirstChild;

public:
  ChildTaskStatusRecord(AsyncTask *child)
      : TaskStatusRecord(TaskStatusRecordKind::ChildTask), FirstChild(child) {}

  ChildTaskStatusRecord(AsyncTask *child, TaskStatusRecordKind kind)
      : TaskStatusRecord(kind), FirstChild(child) {
    assert(kind == TaskStatusRecordKind::ChildTask);
    assert(!child->hasGroupChildFragment() &&
           "Group child tasks must be tracked in their respective "
           "TaskGroupTaskStatusRecord, and not as independent "
           "ChildTaskStatusRecord "
           "records.");
  }

  /// Return the first child linked by this record.  This may be null;
  /// if not, it (and all of its successors) are guaranteed to satisfy
  /// `isChildTask()`.
  AsyncTask *getFirstChild() const { return FirstChild; }

  static AsyncTask *getNextChildTask(AsyncTask *task) {
    return task->childFragment()->getNextChild();
  }

  using child_iterator = LinkedListIterator<AsyncTask, getNextChildTask>;
  llvm::iterator_range<child_iterator> children() const {
    return child_iterator::rangeBeginning(getFirstChild());
  }

  static bool classof(const TaskStatusRecord *record) {
    return record->getKind() == TaskStatusRecordKind::ChildTask;
  }
};

/// A status record which states that a task has a task group.
///
/// A record always is a specific `TaskGroupImpl`.
///
/// This record holds references to all the non-completed children of
/// the task group.  It may also hold references to completed children
/// which have not yet been found by `next()`.
///
/// The child tasks are stored as an invasive single-linked list, starting
/// from `FirstChild` and continuing through the `NextChild` pointers of all
/// the linked children.
///
/// This list structure should only ever be modified:
/// - while holding the status record lock of the owning task, so that
///   asynchronous operations such as cancellation can walk the structure
///   without having to acquire a secondary lock, and
/// - synchronously with the owning task, so that the owning task doesn't
///   have to acquire the status record lock just to walk the structure
///   itself.
///
/// When the group exits, it may simply remove this single record from the task
/// running it, as it has guaranteed that the tasks have already completed.
///
/// Group child tasks DO NOT have their own `ChildTaskStatusRecord` entries,
/// and are only tracked by their respective `TaskGroupTaskStatusRecord`.
class TaskGroupTaskStatusRecord : public TaskStatusRecord {
  AsyncTask *FirstChild;
  AsyncTask *LastChild;

public:
  TaskGroupTaskStatusRecord()
      : TaskStatusRecord(TaskStatusRecordKind::TaskGroup),
        FirstChild(nullptr),
        LastChild(nullptr) {
  }

  TaskGroupTaskStatusRecord(AsyncTask *child)
      : TaskStatusRecord(TaskStatusRecordKind::TaskGroup),
        FirstChild(child),
        LastChild(child) {
    assert(!LastChild || !LastChild->childFragment()->getNextChild());
  }

  TaskGroup *getGroup() { return reinterpret_cast<TaskGroup *>(this); }

  /// Return the first child linked by this record.  This may be null;
  /// if not, it (and all of its successors) are guaranteed to satisfy
  /// `isChildTask()`.
  AsyncTask *getFirstChild() const { return FirstChild; }

  /// Attach the passed in `child` task to this group.
  void attachChild(AsyncTask *child) {
    assert(child->hasGroupChildFragment());
    assert(child->groupChildFragment()->getGroup() == getGroup());

    auto oldLastChild = LastChild;
    LastChild = child;

    if (!FirstChild) {
      // This is the first child we ever attach, so store it as FirstChild.
      FirstChild = child;
      return;
    }

    oldLastChild->childFragment()->setNextChild(child);
  }

  void detachChild(AsyncTask *child) {
    assert(child && "cannot remove a null child from group");
    if (FirstChild == child) {
      FirstChild = getNextChildTask(child);
      if (FirstChild == nullptr) {
        LastChild = nullptr;
      }
      return;
    }

    AsyncTask *prev = FirstChild;
    // Remove the child from the linked list, i.e.:
    //     prev -> afterPrev -> afterChild
    //                 ==
    //               child   -> afterChild
    // Becomes:
    //     prev --------------> afterChild
    while (prev) {
      auto afterPrev = getNextChildTask(prev);

      if (afterPrev == child) {
        auto afterChild = getNextChildTask(child);
        prev->childFragment()->setNextChild(afterChild);
        if (child == LastChild) {
          LastChild = prev;
        }
        return;
      }

      prev = afterPrev;
    }
  }

  static AsyncTask *getNextChildTask(AsyncTask *task) {
    return task->childFragment()->getNextChild();
  }

  using child_iterator = LinkedListIterator<AsyncTask, getNextChildTask>;
  llvm::iterator_range<child_iterator> children() const {
    return child_iterator::rangeBeginning(getFirstChild());
  }

  static bool classof(const TaskStatusRecord *record) {
    return record->getKind() == TaskStatusRecordKind::TaskGroup;
  }
};

/// A cancellation record which states that a task has an arbitrary
/// function that needs to be called if the task is cancelled.
///
/// The end of any call to the function will be ordered before the
/// end of a call to unregister this record from the task.  That is,
/// code may call `removeStatusRecord` and freely
/// assume after it returns that this function will not be
/// subsequently used.
class CancellationNotificationStatusRecord : public TaskStatusRecord {
public:
  using FunctionType = SWIFT_CC(swift) void(SWIFT_CONTEXT void *);

private:
  FunctionType *__ptrauth_swift_cancellation_notification_function Function;
  void *Argument;

public:
  CancellationNotificationStatusRecord(FunctionType *fn, void *arg)
      : TaskStatusRecord(TaskStatusRecordKind::CancellationNotification),
        Function(fn), Argument(arg) {}

  void run() { Function(Argument); }

  static bool classof(const TaskStatusRecord *record) {
    return record->getKind() == TaskStatusRecordKind::CancellationNotification;
  }
};

/// A status record which says that a task has an arbitrary
/// function that needs to be called if the task's priority is escalated.
///
/// The end of any call to the function will be ordered before the
/// end of a call to unregister this record from the task.  That is,
/// code may call `removeStatusRecord` and freely
/// assume after it returns that this function will not be
/// subsequently used.
class EscalationNotificationStatusRecord : public TaskStatusRecord {
public:
  using FunctionType = void(void *, JobPriority);

private:
  FunctionType *__ptrauth_swift_escalation_notification_function Function;
  void *Argument;

public:
  EscalationNotificationStatusRecord(FunctionType *fn, void *arg)
      : TaskStatusRecord(TaskStatusRecordKind::EscalationNotification),
        Function(fn), Argument(arg) {}

  void run(JobPriority newPriority) { Function(Argument, newPriority); }

  static bool classof(const TaskStatusRecord *record) {
    return record->getKind() == TaskStatusRecordKind::EscalationNotification;
  }
};


// TODO (rokhinip): This should probably be part of every task instead of being
// allocated on demand in the task when it first suspends
//
// This record is allocated for a task to record what it is dependent on before
// the task can make progress again.
class TaskDependencyStatusRecord : public TaskStatusRecord {
  // Enum specifying the type of dependency this task has
  enum {
    WaitingOnContinuation,
    WaitingOnTask,
    WaitingOnTaskGroup,
    WaitingOnActor
  } DependencyKind;

  // A word sized storage which references what this task is suspended waiting
  // for. Note that this is different from the waitQueue in the future fragment
  // of a task since that denotes all the tasks which this specific task, will
  // unblock.
  //
  // This field is only really pointing to something valid when the
  // ActiveTaskStatus specifies that the task is suspended. It can be accessed
  // asynchronous to the task due to escalation which will therefore require the
  // task status record lock for synchronization.
  //
  // The type of thing we are waiting on, is specified in the enum above
  union {
    // This task is suspended waiting on a continuation resumption - most
    // likely from from a non-swift-async callback API which will resume it
    // The continuation it is waiting on, is really in this task itself. There
    // are no ref counts managed here - this is just a convenience pointer to
    // the ContinuationAsyncContext in the current task.
    ContinuationAsyncContext *Continuation;

    // This task is suspended waiting on another task. This could be an async
    // let child task or it could be another unstructured task.
    //
    // When this is set, a +1 is taken on the task that we are waiting on.
    // The only fields we can reasonably look at in the task, is the
    // ActiveTaskStatus and its TaskStatusRecords if any.
    AsyncTask *Task;

    // This task is suspended on the task group that it has spawned - we hit
    // this case if the parent task is waiting on pending child tasks in the
    // task group to return results. See also TaskGroupImpl::poll.
    TaskGroup *TaskGroup;

    // This task is suspended waiting on an actor. This implies that
    // we hit contention while trying to access an actor.
    //
    // This field is set for as long as the task is in the actor's job queue
    // - therefore we shouldn't need a separate +1 on the actor, we are
    // borrowing the task's reference on the actor
    DefaultActor *Actor;
  } WaitingOn;

public:
  TaskDependencyStatusRecord(ContinuationAsyncContext *continuation) :
    TaskStatusRecord(TaskStatusRecordKind::TaskDependency),
        DependencyKind(WaitingOnContinuation) {
      WaitingOn.Continuation = continuation;
  }

  TaskDependencyStatusRecord(AsyncTask *task) :
    TaskStatusRecord(TaskStatusRecordKind::TaskDependency),
        DependencyKind(WaitingOnTask) {
      // Released when this record is removed from the active task status
      swift_retain(task);
      WaitingOn.Task = task;
  }

  TaskDependencyStatusRecord(TaskGroup *taskGroup) :
    TaskStatusRecord(TaskStatusRecordKind::TaskDependency),
        DependencyKind(WaitingOnTaskGroup) {
      WaitingOn.TaskGroup = taskGroup;
  }

  TaskDependencyStatusRecord(DefaultActor *actor) :
    TaskStatusRecord(TaskStatusRecordKind::TaskDependency),
        DependencyKind(WaitingOnActor) {
      WaitingOn.Actor = actor;
  }

  void performEscalationAction(JobPriority newPriority) {
    switch (DependencyKind) {
    case WaitingOnContinuation:
      // We can't do anything here
      //
      // TODO (rokhinip): Drop a signpost indicating a potential priority
      // inversion here
      break;
    case WaitingOnTaskGroup:
      // Shortcircuit here. We know that this task will also have a
      // TaskGroupTaskStatusRecord which will handle the escalation logic for
      // the task group
      break;
    case WaitingOnTask:
      // This might be redundant if we are waiting on an async let child task
      // since we'd normally hit it by virtue of esclating all structured
      // concurrency children but the 2nd escalation should just end up
      // shortcircuiting.
      //
      // This is particularly relevant if we are waiting on a task that is not a
      // structured concurrency child task
      swift_task_escalate(WaitingOn.Task, newPriority);
      break;
    case WaitingOnActor:
      // TODO (rokhinip): Escalate the actor which might be running at a lower
      // priority
      swift_actor_escalate(WaitingOn.Actor, task, newPriority);
      break;
    }
  }
};

} // end namespace swift

#endif
