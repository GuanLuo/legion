/* Copyright 2016 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "legion.h"
#include "runtime.h"
#include "legion_ops.h"
#include "legion_tasks.h"
#include "region_tree.h"
#include "legion_spy.h"
#include "legion_profiling.h"
#include "legion_instances.h"
#include "legion_views.h"
#include "legion_analysis.h"

namespace Legion {
  namespace Internal {

    LEGION_EXTERN_LOGGER_DECLARATIONS

    /////////////////////////////////////////////////////////////
    // LogicalView 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LogicalView::LogicalView(RegionTreeForest *ctx, DistributedID did,
                             AddressSpaceID own_addr, AddressSpace loc_space,
                             RegionTreeNode *node)
      : DistributedCollectable(ctx->runtime, did, own_addr, loc_space), 
        context(ctx), logical_node(node), 
        view_lock(Reservation::create_reservation()) 
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LogicalView::~LogicalView(void)
    //--------------------------------------------------------------------------
    {
      view_lock.destroy_reservation();
      view_lock = Reservation::NO_RESERVATION;
    }

    //--------------------------------------------------------------------------
    /*static*/ void LogicalView::delete_logical_view(LogicalView *view)
    //--------------------------------------------------------------------------
    {
      if (view->is_instance_view())
      {
        InstanceView *inst_view = view->as_instance_view();
        if (inst_view->is_materialized_view())
          legion_delete(inst_view->as_materialized_view());
        else if (inst_view->is_reduction_view())
          legion_delete(inst_view->as_reduction_view());
        else
          assert(false);
      }
      else if (view->is_deferred_view())
      {
        DeferredView *deferred_view = view->as_deferred_view();
        if (deferred_view->is_composite_view())
          legion_delete(deferred_view->as_composite_view());
        else if (deferred_view->is_fill_view())
          legion_delete(deferred_view->as_fill_view());
        else
          assert(false);
      }
      else
        assert(false);
    }

    //--------------------------------------------------------------------------
    /*static*/ void LogicalView::handle_view_request(Deserializer &derez,
                                        Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_HIGH_LEVEL
      LogicalView *view = dynamic_cast<LogicalView*>(dc);
      assert(view != NULL);
#else
      LogicalView *view = static_cast<LogicalView*>(dc);
#endif
      view->send_view(source);
    }

    //--------------------------------------------------------------------------
    void LogicalView::defer_collect_user(Event term_event) 
    //--------------------------------------------------------------------------
    {
      // The runtime will add the gc reference to this view when necessary
      runtime->defer_collect_user(this, term_event);
    }
 
    //--------------------------------------------------------------------------
    /*static*/ void LogicalView::handle_deferred_collect(LogicalView *view,
                                             const std::set<Event> &term_events)
    //--------------------------------------------------------------------------
    {
      view->collect_users(term_events);
      // Then remove the gc reference on the object
      if (view->remove_base_gc_ref(PENDING_GC_REF))
        delete_logical_view(view);
    }

    /////////////////////////////////////////////////////////////
    // InstanceView 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    InstanceView::InstanceView(RegionTreeForest *ctx, DistributedID did,
                               AddressSpaceID owner_sp, AddressSpaceID local_sp,
                               RegionTreeNode *node, SingleTask *own_ctx)
      : LogicalView(ctx, did, owner_sp, local_sp, node),
        owner_context(own_ctx)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    InstanceView::~InstanceView(void)
    //--------------------------------------------------------------------------
    { 
    }

    /////////////////////////////////////////////////////////////
    // MaterializedView 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    MaterializedView::MaterializedView(
                               RegionTreeForest *ctx, DistributedID did,
                               AddressSpaceID own_addr, AddressSpaceID loc_addr,
                               RegionTreeNode *node, InstanceManager *man,
                               MaterializedView *par, SingleTask *own_ctx)
      : InstanceView(ctx, encode_materialized_did(did, par == NULL), own_addr, 
                     loc_addr, node, own_ctx), manager(man), parent(par)
    //--------------------------------------------------------------------------
    {
      // Otherwise the instance lock will get filled in when we are unpacked
#ifdef DEBUG_HIGH_LEVEL
      assert(manager != NULL);
#endif
      logical_node->register_instance_view(manager, owner_context, this);
      // If we are either not a parent or we are a remote parent add 
      // a resource reference to avoid being collected
      if (parent != NULL)
        add_nested_resource_ref(did);
      else 
      {
        manager->add_nested_resource_ref(did);
        // If we are the root and remote add a resource reference from
        // the owner node
        if (!is_owner())
          add_base_resource_ref(REMOTE_DID_REF);
      }
#ifdef LEGION_GC
      log_garbage.info("GC Materialized View %ld %ld", did, manager->did); 
#endif
    }

    //--------------------------------------------------------------------------
    MaterializedView::MaterializedView(const MaterializedView &rhs)
      : InstanceView(NULL, 0, 0, 0, NULL, NULL),
        manager(NULL), parent(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    MaterializedView::~MaterializedView(void)
    //--------------------------------------------------------------------------
    {
      // Always unregister ourselves with the region tree node
      logical_node->unregister_instance_view(manager, owner_context);
      // Remove our resource references on our children
      // Capture their recycle events in the process
      for (std::map<ColorPoint,MaterializedView*>::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        recycle_events.insert(it->second->get_destruction_event());
        if (it->second->remove_nested_resource_ref(did))
          legion_delete(it->second);
      }
      if (parent == NULL)
      {
        if (manager->remove_nested_resource_ref(did))
          delete manager;
        if (is_owner())
        {
          UpdateReferenceFunctor<RESOURCE_REF_KIND,false/*add*/> functor(this);
          map_over_remote_instances(functor);
        }
      }
      if (!atomic_reservations.empty())
      {
        // If this is the owner view, delete any atomic reservations
        if (is_owner())
        {
          for (std::map<FieldID,Reservation>::iterator it = 
                atomic_reservations.begin(); it != 
                atomic_reservations.end(); it++)
          {
            it->second.destroy_reservation();
          }
        }
        atomic_reservations.clear();
      }
      if (!initial_user_events.empty())
      {
        for (std::set<Event>::const_iterator it = initial_user_events.begin();
              it != initial_user_events.end(); it++)
          filter_local_users(*it);
      }
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE) && \
      defined(DEBUG_HIGH_LEVEL)
      // Don't forget to remove the initial user if there was one
      // before running these checks
      assert(current_epoch_users.empty());
      assert(previous_epoch_users.empty());
      assert(outstanding_gc_events.empty());
#endif
    }

    //--------------------------------------------------------------------------
    MaterializedView& MaterializedView::operator=(const MaterializedView &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    Memory MaterializedView::get_location(void) const
    //--------------------------------------------------------------------------
    {
      return manager->get_memory();
    }

    //--------------------------------------------------------------------------
    const FieldMask& MaterializedView::get_physical_mask(void) const
    //--------------------------------------------------------------------------
    {
      return manager->layout->allocated_fields;
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::has_space(const FieldMask &space_mask) const
    //--------------------------------------------------------------------------
    {
      return !(space_mask - manager->layout->allocated_fields);
    }

    //--------------------------------------------------------------------------
    LogicalView* MaterializedView::get_subview(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      return get_materialized_subview(c);
    }

    //--------------------------------------------------------------------------
    MaterializedView* MaterializedView::get_materialized_subview(
                                                           const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // This is the common case we should already have it
      {
        AutoLock v_lock(view_lock, 1, false/*exclusive*/);
        std::map<ColorPoint,MaterializedView*>::const_iterator finder = 
                                                            children.find(c);
        if (finder != children.end())
          return finder->second;
      }
      // If we don't have it, we have to make it
      if (is_owner())
      {
        RegionTreeNode *child_node = logical_node->get_tree_child(c);
        // Allocate the DID eagerly
        DistributedID child_did = 
          context->runtime->get_available_distributed_id(false);
        bool free_child_did = false;
        MaterializedView *child_view = NULL;
        {
          // Retake the lock and see if we lost the race
          AutoLock v_lock(view_lock);
          std::map<ColorPoint,MaterializedView*>::const_iterator finder = 
                                                              children.find(c);
          if (finder != children.end())
          {
            child_view = finder->second;
            free_child_did = true;
          }
          else
          {
            // Otherwise we get to make it
            child_view = legion_new<MaterializedView>(context, child_did, 
                                              owner_space, local_space,
                                              child_node, manager, this,
                                              owner_context);
            children[c] = child_view;
          }
          if (free_child_did)
            context->runtime->free_distributed_id(child_did);
          return child_view;
        }
      }
      else
      {
        // Find the distributed ID for this child view
        volatile DistributedID child_did;
        UserEvent wait_on = UserEvent::create_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize(c);
          rez.serialize(&child_did);
          rez.serialize(wait_on);
        }
        runtime->send_subview_did_request(owner_space, rez); 
        wait_on.wait();
        Event ready = Event::NO_EVENT;
        LogicalView *child_view = 
          context->runtime->find_or_request_logical_view(child_did, ready);
        if (ready.exists())
          ready.wait();
#ifdef DEBUG_HIGH_LEVEL
        assert(child_view->is_materialized_view());
#endif
        MaterializedView *mat_child = child_view->as_materialized_view();
        // Retake the lock and add the child
        AutoLock v_lock(view_lock);
        children[c] = mat_child;
        return mat_child;
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void MaterializedView::handle_subview_did_request(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID parent_did;
      derez.deserialize(parent_did);
      ColorPoint color;
      derez.deserialize(color);
      DistributedID *target;
      derez.deserialize(target);
      UserEvent to_trigger;
      derez.deserialize(to_trigger);
      DistributedCollectable *dc = 
        runtime->find_distributed_collectable(parent_did);
#ifdef DEBUG_HIGH_LEVEL
      MaterializedView *parent_view = dynamic_cast<MaterializedView*>(dc);
      assert(parent_view != NULL);
#else
      MaterializedView *parent_view = static_cast<MaterializedView*>(dc);
#endif
      MaterializedView *child_view = 
        parent_view->get_materialized_subview(color);
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(child_view->did);
        rez.serialize(target);
        rez.serialize(to_trigger);
      }
      runtime->send_subview_did_response(source, rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void MaterializedView::handle_subview_did_response(
                                                            Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID result;
      derez.deserialize(result);
      DistributedID *target;
      derez.deserialize(target);
      UserEvent to_trigger;
      derez.deserialize(to_trigger);
      (*target) = result;
      to_trigger.trigger();
    }

    //--------------------------------------------------------------------------
    MaterializedView* MaterializedView::get_materialized_parent_view(void) const
    //--------------------------------------------------------------------------
    {
      return parent;
    }

    //--------------------------------------------------------------------------
    void MaterializedView::copy_field(FieldID fid,
                              std::vector<Domain::CopySrcDstField> &copy_fields)
    //--------------------------------------------------------------------------
    {
      std::vector<FieldID> local_fields(1,fid);
      manager->compute_copy_offsets(local_fields, copy_fields); 
    }

    //--------------------------------------------------------------------------
    void MaterializedView::copy_to(const FieldMask &copy_mask,
                               std::vector<Domain::CopySrcDstField> &dst_fields,
                                   CopyAcrossHelper *across_helper)
    //--------------------------------------------------------------------------
    {
      if (across_helper == NULL)
        manager->compute_copy_offsets(copy_mask, dst_fields);
      else
        across_helper->compute_across_offsets(copy_mask, dst_fields);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::copy_from(const FieldMask &copy_mask,
                               std::vector<Domain::CopySrcDstField> &src_fields)
    //--------------------------------------------------------------------------
    {
      manager->compute_copy_offsets(copy_mask, src_fields);
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::reduce_to(ReductionOpID redop, 
                                     const FieldMask &copy_mask,
                               std::vector<Domain::CopySrcDstField> &dst_fields,
                                     CopyAcrossHelper *across_helper)
    //--------------------------------------------------------------------------
    {
      if (across_helper == NULL)
        manager->compute_copy_offsets(copy_mask, dst_fields);
      else
        across_helper->compute_across_offsets(copy_mask, dst_fields);
      return false; // not a fold
    }

    //--------------------------------------------------------------------------
    void MaterializedView::reduce_from(ReductionOpID redop,
                                       const FieldMask &reduce_mask,
                               std::vector<Domain::CopySrcDstField> &src_fields)
    //--------------------------------------------------------------------------
    {
      manager->compute_copy_offsets(reduce_mask, src_fields);
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::has_war_dependence(const RegionUsage &usage,
                                              const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      // No WAR dependences for read-only or reduce 
      if (IS_READ_ONLY(usage) || IS_REDUCE(usage))
        return false;
      const ColorPoint &local_color = logical_node->get_color();
      if (has_local_war_dependence(usage, user_mask, ColorPoint(), local_color))
        return true;
      if (parent != NULL)
        return parent->has_war_dependence_above(usage, user_mask, local_color);
      return false;
    } 

    //--------------------------------------------------------------------------
    void MaterializedView::accumulate_events(std::set<Event> &all_events)
    //--------------------------------------------------------------------------
    {
      AutoLock v_lock(view_lock,1,false/*exclusive*/);
      all_events.insert(outstanding_gc_events.begin(),
                        outstanding_gc_events.end());
    } 

    //--------------------------------------------------------------------------
    void MaterializedView::add_copy_user(ReductionOpID redop, Event copy_term,
                                         const VersionInfo &version_info,
                                     const FieldMask &copy_mask, bool reading)
    //--------------------------------------------------------------------------
    {
      // Quick test, we only need to do this if the copy_term event
      // exists, otherwise the user is already done
      if (copy_term.exists())
      {
        RegionUsage usage;
        usage.redop = redop;
        usage.prop = EXCLUSIVE;
        if (reading)
          usage.privilege = READ_ONLY;
        else if (redop > 0)
          usage.privilege = REDUCE;
        else
          usage.privilege = READ_WRITE;
        if ((parent != NULL) && !version_info.is_upper_bound_node(logical_node))
        {
          const ColorPoint &local_color = logical_node->get_color();
          parent->add_copy_user_above(usage, copy_term, local_color,
                                      version_info, copy_mask);
        }
        add_local_copy_user(usage, copy_term, true/*base*/, ColorPoint(),
                            version_info, copy_mask);
      }
    }

    //--------------------------------------------------------------------------
    Event MaterializedView::add_user(const RegionUsage &usage, Event term_event,
                                     const FieldMask &user_mask, Operation *op,
                                     const VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
      std::set<Event> wait_on_events;
      Event start_use_event = manager->get_use_event();
      if (start_use_event.exists())
        wait_on_events.insert(start_use_event);
      if ((parent != NULL) && !version_info.is_upper_bound_node(logical_node))
      {
        const ColorPoint &local_color = logical_node->get_color();
        parent->add_user_above(usage, term_event, local_color,
                               version_info, user_mask, wait_on_events);
      }
      const bool issue_collect = add_local_user(usage, term_event, true/*base*/,
                         ColorPoint(), version_info, user_mask, wait_on_events);
      // Launch the garbage collection task, if it doesn't exist
      // then the user wasn't registered anyway, see add_local_user
      if (issue_collect)
        defer_collect_user(term_event);
      // At this point tasks shouldn't be allowed to wait on themselves
#ifdef DEBUG_HIGH_LEVEL
      if (term_event.exists())
        assert(wait_on_events.find(term_event) == wait_on_events.end());
#endif
      if (IS_ATOMIC(usage))
        find_atomic_reservations(user_mask, op, IS_WRITE(usage));
      // Return the merge of the events
      return Runtime::merge_events<false>(wait_on_events);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::add_initial_user(Event term_event,
                                            const RegionUsage &usage,
                                            const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      // No need to take the lock since we are just initializing
      PhysicalUser *user = legion_new<PhysicalUser>(usage, ColorPoint());
      user->add_reference();
      add_current_user(user, term_event, user_mask);
      initial_user_events.insert(term_event);
      // Don't need to actual launch a collection task, destructor
      // will handle this case
      outstanding_gc_events.insert(term_event);
    }
 
    //--------------------------------------------------------------------------
    void MaterializedView::notify_active(void)
    //--------------------------------------------------------------------------
    {
      if (parent == NULL)
        manager->add_nested_gc_ref(did);
      else
        parent->add_nested_gc_ref(did);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::notify_inactive(void)
    //--------------------------------------------------------------------------
    {
      // No need to worry about handling the deletion case since
      // we know we also hold a resource reference and therefore
      // the manager won't be deleted until we are deleted at
      // the earliest
      if (parent == NULL)
        manager->remove_nested_gc_ref(did);
      else if (parent->remove_nested_gc_ref(did))
        delete parent;
    }

    //--------------------------------------------------------------------------
    void MaterializedView::notify_valid(void)
    //--------------------------------------------------------------------------
    {
      // If we are at the top of the tree add a valid reference
      // Otherwise add our valid reference on our parent
      if (parent == NULL)
      {
        if (!is_owner())
          send_remote_valid_update(owner_space, 1/*count*/, true/*add*/);
        manager->add_nested_valid_ref(did);
      }
      else
        parent->add_nested_valid_ref(did);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::notify_invalid(void)
    //--------------------------------------------------------------------------
    {
      // If we are at the top of the tree add a valid reference
      // Otherwise add our valid reference on the parent
      if (parent == NULL)
      {
        if (!is_owner())
          send_remote_valid_update(owner_space, 1/*count*/, false/*add*/);
        manager->remove_nested_valid_ref(did);
      }
      else if (parent->remove_nested_valid_ref(did))
        legion_delete(parent);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::collect_users(const std::set<Event> &term_events)
    //--------------------------------------------------------------------------
    {
      {
        AutoLock v_lock(view_lock);
        // Remove any event users from the current and previous users
        for (std::set<Event>::const_iterator it = term_events.begin();
              it != term_events.end(); it++)
        {
          filter_local_users(*it); 
        }
      }
      if (parent != NULL)
        parent->collect_users(term_events);
    } 

    //--------------------------------------------------------------------------
    void MaterializedView::send_view(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(is_owner());
      assert(logical_node->is_region());
#endif
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(did);
        rez.serialize(manager->did);
        if (parent == NULL)
          rez.serialize<DistributedID>(0);
        else
          rez.serialize<DistributedID>(parent->did);
        rez.serialize(logical_node->as_region_node()->handle);
        rez.serialize(owner_space);
        rez.serialize<UniqueID>(owner_context->get_context_id());
      }
      runtime->send_materialized_view(target, rez);
      update_remote_instances(target); 
    }

    //--------------------------------------------------------------------------
    void MaterializedView::send_view_updates(AddressSpaceID target,
                                             const FieldMask &update_mask)
    //--------------------------------------------------------------------------
    {
      std::map<PhysicalUser*,int/*index*/> needed_users;  
      Serializer current_rez, previous_rez;
      unsigned current_events = 0, previous_events = 0;
      // Take the lock in read-only mode
      {
        AutoLock v_lock(view_lock,1,false/*exclusive*/);
        for (LegionMap<Event,EventUsers>::aligned::const_iterator cit = 
              current_epoch_users.begin(); cit != 
              current_epoch_users.end(); cit++)
        {
          FieldMask overlap = cit->second.user_mask & update_mask;
          if (!overlap)
            continue;
          current_events++;
          current_rez.serialize(cit->first);
          const EventUsers &event_users = cit->second;
          if (event_users.single)
          {
            int index = needed_users.size();
            needed_users[event_users.users.single_user] = index;
            event_users.users.single_user->add_reference();
            current_rez.serialize(index);
            current_rez.serialize(overlap);
          }
          else
          {
            Serializer event_rez;
            int count = -1; // start this at negative one
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                  it = event_users.users.multi_users->begin(); it != 
                  event_users.users.multi_users->end(); it++)
            {
              FieldMask overlap2 = it->second & overlap;
              if (!overlap2)
                continue;
              count--; // Make the count negative to disambiguate
              int index = needed_users.size();
              needed_users[it->first] = index;
              it->first->add_reference();
              event_rez.serialize(index);
              event_rez.serialize(overlap2);
            }
            // If there was only one, we can take the normal path
            if ((count == -1) || (count < -2))
              current_rez.serialize(count);
            size_t event_rez_size = event_rez.get_used_bytes();
            current_rez.serialize(event_rez.get_buffer(), event_rez_size);
          }
        }
        for (LegionMap<Event,EventUsers>::aligned::const_iterator pit = 
              previous_epoch_users.begin(); pit != 
              previous_epoch_users.end(); pit++)
        {
          FieldMask overlap = pit->second.user_mask & update_mask;
          if (!overlap)
            continue;
          previous_events++;
          previous_rez.serialize(pit->first);
          const EventUsers &event_users = pit->second;
          if (event_users.single)
          {
            std::map<PhysicalUser*,int>::const_iterator finder = 
              needed_users.find(event_users.users.single_user);
            if (finder == needed_users.end())
            {
              int index = needed_users.size();
              previous_rez.serialize(index);
              needed_users[event_users.users.single_user] = index;
              event_users.users.single_user->add_reference();
            }
            else
              previous_rez.serialize(finder->second);
            previous_rez.serialize(overlap);
          }
          else 
          {
            Serializer event_rez;
            int count = -1; // start this at negative one
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator
                  it = event_users.users.multi_users->begin(); it !=
                  event_users.users.multi_users->end(); it++)
            {
              FieldMask overlap2 = it->second & overlap;
              if (!overlap2)
                continue;
              count--; // Make the count negative to disambiguate
              std::map<PhysicalUser*,int>::const_iterator finder = 
                needed_users.find(it->first);
              if (finder == needed_users.end())
              {
                int index = needed_users.size();
                needed_users[it->first] = index;
                event_rez.serialize(index);
                it->first->add_reference();
              }
              else
                event_rez.serialize(finder->second);
              event_rez.serialize(overlap2);
            }
            // If there was only one user, we can take the normal path
            if ((count == -1) || (count < -2))
              previous_rez.serialize(count);
            size_t event_rez_size = event_rez.get_used_bytes();
            previous_rez.serialize(event_rez.get_buffer(), event_rez_size); 
          }
        }
      }
      // Now build our buffer and send the result
      Serializer rez;
      {
        RezCheck z(rez);
        bool is_region = logical_node->is_region();
        rez.serialize(is_region);
        if (is_region)
          rez.serialize(logical_node->as_region_node()->handle);
        else
          rez.serialize(logical_node->as_partition_node()->handle);
        rez.serialize(did);
        // Pack the needed users first
        rez.serialize<size_t>(needed_users.size());
        for (std::map<PhysicalUser*,int>::const_iterator it = 
              needed_users.begin(); it != needed_users.end(); it++)
        {
          rez.serialize(it->second);
          it->first->pack_user(rez);
          if (it->first->remove_reference())
            legion_delete(it->first);
        }
        // Then pack the current and previous events
        rez.serialize(current_events);
        size_t current_size = current_rez.get_used_bytes();
        rez.serialize(current_rez.get_buffer(), current_size);
        rez.serialize(previous_events);
        size_t previous_size = previous_rez.get_used_bytes();
        rez.serialize(previous_rez.get_buffer(), previous_size);
      }
      runtime->send_materialized_update(target, rez);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::process_update(Deserializer &derez,
                                          AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      size_t num_users;
      derez.deserialize(num_users);
      std::vector<PhysicalUser*> users(num_users);
      FieldSpaceNode *field_node = logical_node->column_source;
      for (unsigned idx = 0; idx < num_users; idx++)
      {
        int index;
        derez.deserialize(index);
        users[index] = PhysicalUser::unpack_user(derez, field_node, 
                                                 source, true/*add ref*/); 
      }
      // We've already added a reference for all users since we'll know
      // that we'll be adding them at least once
      std::vector<bool> need_reference(num_users, false);
      std::deque<Event> collect_events;
      {
        // Hold the lock when updating the view
        AutoLock v_lock(view_lock); 
        unsigned num_current;
        derez.deserialize(num_current);
        for (unsigned idx = 0; idx < num_current; idx++)
        {
          Event current_event;
          derez.deserialize(current_event);
          int index;
          derez.deserialize(index);
          if (index < 0)
          {
            int count = (-index) - 1;
            for (int i = 0; i < count; i++)
            {
              derez.deserialize(index);
#ifdef DEBUG_HIGH_LEVEL
              assert(unsigned(index) < num_users);
#endif
              FieldMask user_mask;
              derez.deserialize(user_mask);
              if (need_reference[index])
                users[index]->add_reference();
              else
                need_reference[index] = true;
              add_current_user(users[index], current_event, user_mask);
            }
          }
          else
          {
#ifdef DEBUG_HIGH_LEVEL
            assert(unsigned(index) < num_users);
#endif
            // Just one user
            FieldMask user_mask;
            derez.deserialize(user_mask);
            if (need_reference[index])
              users[index]->add_reference();
            else
              need_reference[index] = true;
            add_current_user(users[index], current_event, user_mask);
          }
          if (outstanding_gc_events.find(current_event) ==
              outstanding_gc_events.end())
          {
            outstanding_gc_events.insert(current_event);
            collect_events.push_back(current_event);
          }
        }
        unsigned num_previous;
        derez.deserialize(num_previous);
        for (unsigned idx = 0; idx < num_previous; idx++)
        {
          Event previous_event;
          derez.deserialize(previous_event);
          int index;
          derez.deserialize(index);
          if (index < 0)
          {
            int count = (-index) - 1;
            for (int i = 0; i < count; i++)
            {
              derez.deserialize(index);
#ifdef DEBUG_HIGH_LEVEL
              assert(unsigned(index) < num_users);
#endif
              FieldMask user_mask;
              derez.deserialize(user_mask);
              if (need_reference[index])
                users[index]->add_reference();
              else
                need_reference[index] = true;
              add_previous_user(users[index], previous_event, user_mask);
            }
          }
          else
          {
#ifdef DEBUG_HIGH_LEVEL
            assert(unsigned(index) < num_users);
#endif
            // Just one user
            FieldMask user_mask;
            derez.deserialize(user_mask);
            if (need_reference[index])
              users[index]->add_reference();
            else
              need_reference[index] = true;
            add_previous_user(users[index], previous_event, user_mask);
          }
          if (outstanding_gc_events.find(previous_event) ==
              outstanding_gc_events.end())
          {
            outstanding_gc_events.insert(previous_event);
            collect_events.push_back(previous_event);
          }
        }
      }
      if (!collect_events.empty())
      {
        if (parent != NULL)
          parent->update_gc_events(collect_events);
        for (std::deque<Event>::const_iterator it = 
              collect_events.begin(); it != collect_events.end(); it++)
        {
          defer_collect_user(*it); 
        }
      }
#ifdef DEBUG_HIGH_LEVEL
      for (unsigned idx = 0; idx < need_reference.size(); idx++)
        assert(need_reference[idx]);
#endif
    }

    //--------------------------------------------------------------------------
    void MaterializedView::update_gc_events(const std::deque<Event> &gc_events)
    //--------------------------------------------------------------------------
    {
      if (parent != NULL)
        parent->update_gc_events(gc_events);
      AutoLock v_lock(view_lock);
      for (std::deque<Event>::const_iterator it = gc_events.begin();
            it != gc_events.end(); it++)
      {
        outstanding_gc_events.insert(*it);
      }
    }

    //--------------------------------------------------------------------------
    void MaterializedView::add_user_above(const RegionUsage &usage, 
                                          Event term_event,
                                          const ColorPoint &child_color,
                                          const VersionInfo &version_info,
                                          const FieldMask &user_mask,
                                          std::set<Event> &preconditions)
    //--------------------------------------------------------------------------
    {
      if ((parent != NULL) && !version_info.is_upper_bound_node(logical_node))
      {
        const ColorPoint &local_color = logical_node->get_color();
        parent->add_user_above(usage, term_event, local_color,
                               version_info, user_mask, preconditions);
      }
      add_local_user(usage, term_event, false/*base*/, child_color,
                     version_info, user_mask, preconditions);
      // No need to launch a collect user task, the child takes care of that
    }

    //--------------------------------------------------------------------------
    void MaterializedView::add_copy_user_above(const RegionUsage &usage, 
                                               Event copy_term, 
                                               const ColorPoint &child_color,
                                               const VersionInfo &version_info,
                                               const FieldMask &copy_mask)
    //--------------------------------------------------------------------------
    {
      if ((parent != NULL) && !version_info.is_upper_bound_node(logical_node))
      {
        const ColorPoint &local_color = logical_node->get_color();
        parent->add_copy_user_above(usage, copy_term, local_color,
                                    version_info, copy_mask);
      }
      add_local_copy_user(usage, copy_term, false/*base*/, child_color, 
                          version_info, copy_mask);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::add_local_copy_user(const RegionUsage &usage, 
                                               Event copy_term, bool base_user,
                                               const ColorPoint &child_color,
                                               const VersionInfo &version_info,
                                               const FieldMask &copy_mask)
    //--------------------------------------------------------------------------
    {
      PhysicalUser *user;
      // We currently only use the version information for avoiding
      // WAR dependences on the same version number, so we don't need
      // it if we aren't only reading
      if (IS_READ_ONLY(usage))
        user = legion_new<PhysicalUser>(usage, child_color,
                                        version_info.get_versions(logical_node));
      else
        user = legion_new<PhysicalUser>(usage, child_color);
      user->add_reference();
      bool issue_collect = false;
      {
        AutoLock v_lock(view_lock);
        add_current_user(user, copy_term, copy_mask); 
        if (base_user)
          issue_collect = (outstanding_gc_events.find(copy_term) ==
                            outstanding_gc_events.end());
        outstanding_gc_events.insert(copy_term);
      }
      if (issue_collect)
        defer_collect_user(copy_term);
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::add_local_user(const RegionUsage &usage,
                                          Event term_event, bool base_user,
                                          const ColorPoint &child_color,
                                          const VersionInfo &version_info,
                                          const FieldMask &user_mask,
                                          std::set<Event> &preconditions)
    //--------------------------------------------------------------------------
    {
      std::set<Event> dead_events;
      LegionMap<Event,FieldMask>::aligned filter_previous;
      FieldMask dominated;
      // Hold the lock in read-only mode when doing this part of the analysis
      {
        AutoLock v_lock(view_lock,1,false/*exclusive*/);
        FieldMask observed, non_dominated;
        for (LegionMap<Event,EventUsers>::aligned::const_iterator cit = 
              current_epoch_users.begin(); cit != 
              current_epoch_users.end(); cit++)
        {
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE)
          // We're about to do a bunch of expensive tests, 
          // so first do something cheap to see if we can 
          // skip all the tests.
          if (cit->first.has_triggered())
          {
            dead_events.insert(cit->first);
            continue;
          }
#endif
          // No need to check for dependences on ourselves
          if (cit->first == term_event)
            continue;
          // If we arleady recorded a dependence, then we are done
          if (preconditions.find(cit->first) != preconditions.end())
            continue;
          const EventUsers &event_users = cit->second;
          if (event_users.single)
          {
            find_current_preconditions(cit->first, 
                                       event_users.users.single_user,
                                       event_users.user_mask,
                                       usage, user_mask, child_color,
                                       preconditions, observed, non_dominated);
          }
          else
          {
            // Otherwise do a quick test for non-interference on the
            // summary mask and iterate the users if needed
            if (!(user_mask * event_users.user_mask))
            {
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                    it = event_users.users.multi_users->begin(); it !=
                    event_users.users.multi_users->end(); it++)
              {
                // Unlike with the copy analysis, once we record a dependence
                // on an event, we are done, so we can keep going
                if (find_current_preconditions(cit->first,
                                               it->first, it->second,
                                               usage, user_mask, child_color,
                                               preconditions, observed, 
                                               non_dominated))
                  break;
              }
            }
          }
        }
        // See if we have any fields for which we need to do an analysis
        // on the previous fields
        // It's only safe to dominate fields that we observed
        dominated = (observed & (user_mask - non_dominated));
        // Update the non-dominated mask with what we
        // we're actually not-dominated by
        non_dominated = user_mask - dominated;
        const bool skip_analysis = !non_dominated;
        for (LegionMap<Event,EventUsers>::aligned::const_iterator pit = 
              previous_epoch_users.begin(); pit != 
              previous_epoch_users.end(); pit++)
        {
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE)
          // We're about to do a bunch of expensive tests, 
          // so first do something cheap to see if we can 
          // skip all the tests.
          if (pit->first.has_triggered())
          {
            dead_events.insert(pit->first);
            continue;
          }
#endif
          // No need to check for dependences on ourselves
          if (pit->first == term_event)
            continue;
          // If we arleady recorded a dependence, then we are done
          if (preconditions.find(pit->first) != preconditions.end())
            continue;
          const EventUsers &event_users = pit->second;
          if (!!dominated)
          {
            FieldMask dom_overlap = event_users.user_mask & dominated;
            if (!!dom_overlap)
              filter_previous[pit->first] = dom_overlap;
          }
          // If we don't have any non-dominated fields we can skip the
          // rest of the analysis because we dominated everything
          if (skip_analysis)
            continue;
          if (event_users.single)
          {
            find_previous_preconditions(pit->first,
                                        event_users.users.single_user,
                                        event_users.user_mask,
                                        usage, non_dominated,
                                        child_color, preconditions);
          }
          else
          {
            if (!(non_dominated * event_users.user_mask))
            {
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                    it = event_users.users.multi_users->begin(); it !=
                    event_users.users.multi_users->end(); it++)
              {
                // Once we find a dependence we are can skip the rest
                if (find_previous_preconditions(pit->first,
                                                it->first, it->second,
                                                usage, non_dominated,
                                                child_color, preconditions))
                  break;
              }
            }
          }
        }
      }
      PhysicalUser *new_user = NULL;
      if (term_event.exists())
      {
        // Only need to record version info if we are read-only
        // because we only use the version info for avoiding WAR dependences
        if (IS_READ_ONLY(usage))
          new_user = legion_new<PhysicalUser>(usage, child_color,
                                      version_info.get_versions(logical_node));
        else
          new_user = legion_new<PhysicalUser>(usage, child_color);
        new_user->add_reference();
      }
      // No matter what, we retake the lock in exclusive mode so we
      // can handle any clean-up and add our user
      AutoLock v_lock(view_lock);
      if (!dead_events.empty())
      {
        for (std::set<Event>::const_iterator it = dead_events.begin();
              it != dead_events.end(); it++)
        {
          filter_local_users(*it);
        }
      }
      if (!filter_previous.empty())
        filter_previous_users(filter_previous);
      if (!!dominated)
        filter_current_users(dominated);
      // Finally add our user and return if we need to issue a GC meta-task
      if (term_event.exists())
      {
        add_current_user(new_user, term_event, user_mask);
        if (outstanding_gc_events.find(term_event) == 
            outstanding_gc_events.end())
        {
          outstanding_gc_events.insert(term_event);
          return base_user;
        }
      }
      return false;
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::find_current_preconditions(Event test_event,
                                                 const PhysicalUser *prev_user,
                                                 const FieldMask &prev_mask,
                                                 const RegionUsage &next_user,
                                                 const FieldMask &next_mask,
                                                 const ColorPoint &child_color,
                                                 std::set<Event> &preconditions,
                                                 FieldMask &observed,
                                                 FieldMask &non_dominated)
    //--------------------------------------------------------------------------
    {
      FieldMask overlap = prev_mask & next_mask;
      if (!overlap)
        return false;
      else
        observed |= overlap;
      if (child_color.is_valid())
      {
        // Same child, already done the analysis
        if (child_color == prev_user->child)
        {
          non_dominated |= overlap;
          return false;
        }
        // Disjoint children, keep going
        if (prev_user->child.is_valid() &&
            logical_node->are_children_disjoint(child_color,
                                                prev_user->child))
        {
          non_dominated |= overlap;
          return false;
        }
      }
      // Now do a dependence analysis
      DependenceType dt = check_dependence_type(prev_user->usage, next_user);
      switch (dt)
      {
        case NO_DEPENDENCE:
        case ATOMIC_DEPENDENCE:
        case SIMULTANEOUS_DEPENDENCE:
          {
            // No actual dependence
            non_dominated |= overlap;
            return false;
          }
        case TRUE_DEPENDENCE:
        case ANTI_DEPENDENCE:
          {
            // Actual dependence
            preconditions.insert(test_event);
            break;
          }
        default:
          assert(false); // should never get here
      }
      // If we made it to the end we recorded a dependence so return true
      return true;
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::find_previous_preconditions(Event test_event,
                                                 const PhysicalUser *prev_user,
                                                 const FieldMask &prev_mask,
                                                 const RegionUsage &next_user,
                                                 const FieldMask &next_mask,
                                                 const ColorPoint &child_color,
                                                 std::set<Event> &preconditions)
    //--------------------------------------------------------------------------
    {
      if (child_color.is_valid())
      {
        // Same child: did analysis below
        if (child_color == prev_user->child)
          return false;
        if (prev_user->child.is_valid() &&
            logical_node->are_children_disjoint(child_color,
                                          prev_user->child))
          return false;
      }
      FieldMask overlap = prev_mask & next_mask;
      if (!overlap)
        return false;
      // Now do a dependence analysis
      DependenceType dt = check_dependence_type(prev_user->usage, next_user);
      switch (dt)
      {
        case NO_DEPENDENCE:
        case ATOMIC_DEPENDENCE:
        case SIMULTANEOUS_DEPENDENCE:
          {
            // No actual dependence
            return false;
          }
        case TRUE_DEPENDENCE:
        case ANTI_DEPENDENCE:
          {
            // Actual dependence
            preconditions.insert(test_event);
            break;
          }
        default:
          assert(false); // should never get here
      }
      // If we make it here, we recorded a dependence
      return true;
    }
 
    //--------------------------------------------------------------------------
    void MaterializedView::find_copy_preconditions(ReductionOpID redop, 
                                                   bool reading, 
                                                   const FieldMask &copy_mask,
                                                const VersionInfo &version_info,
                             LegionMap<Event,FieldMask>::aligned &preconditions)
    //--------------------------------------------------------------------------
    {
      Event start_use_event = manager->get_use_event();
      if (start_use_event.exists())
      {
        LegionMap<Event,FieldMask>::aligned::iterator finder = 
          preconditions.find(start_use_event);
        if (finder == preconditions.end())
          preconditions[start_use_event] = copy_mask;
        else
          finder->second |= copy_mask;
      }
      if ((parent != NULL) && !version_info.is_upper_bound_node(logical_node))
      {
        const ColorPoint &local_point = logical_node->get_color();
        parent->find_copy_preconditions_above(redop, reading, copy_mask,
                                      local_point, version_info, preconditions);
      }
      find_local_copy_preconditions(redop, reading, copy_mask, 
                                    ColorPoint(), version_info, preconditions);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::find_copy_preconditions_above(ReductionOpID redop,
                                                         bool reading,
                                                     const FieldMask &copy_mask,
                                                  const ColorPoint &child_color,
                                                const VersionInfo &version_info,
                             LegionMap<Event,FieldMask>::aligned &preconditions)
    //--------------------------------------------------------------------------
    {
      if ((parent != NULL) && !version_info.is_upper_bound_node(logical_node))
      {
        const ColorPoint &local_point = logical_node->get_color();
        parent->find_copy_preconditions_above(redop, reading, copy_mask,
                                      local_point, version_info, preconditions);
      }
      find_local_copy_preconditions(redop, reading, copy_mask, 
                                    child_color, version_info, preconditions);
    }
    
    //--------------------------------------------------------------------------
    void MaterializedView::find_local_copy_preconditions(ReductionOpID redop,
                                                         bool reading,
                                                     const FieldMask &copy_mask,
                                                  const ColorPoint &child_color,
                                                const VersionInfo &version_info,
                             LegionMap<Event,FieldMask>::aligned &preconditions)
    //--------------------------------------------------------------------------
    {
      // First get our set of version data in case we need it, it's really
      // only safe to do this if we are at the bottom of our set of versions
      const FieldVersions *versions = 
        child_color.is_valid() ? NULL : version_info.get_versions(logical_node);
      std::set<Event> dead_events;
      LegionMap<Event,FieldMask>::aligned filter_previous;
      FieldMask dominated;
      {
        // Hold the lock in read-only mode when doing this analysis
        AutoLock v_lock(view_lock,1,false/*exclusive*/);
        FieldMask observed, non_dominated;
        for (LegionMap<Event,EventUsers>::aligned::const_iterator cit = 
              current_epoch_users.begin(); cit != 
              current_epoch_users.end(); cit++)
        {
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE)
          // We're about to do a bunch of expensive tests, 
          // so first do something cheap to see if we can 
          // skip all the tests.
          if (cit->first.has_triggered())
          {
            dead_events.insert(cit->first);
            continue;
          }
#endif
          const EventUsers &event_users = cit->second;
          if (event_users.single)
          {
            find_current_copy_preconditions(cit->first,
                                            event_users.users.single_user,
                                            event_users.user_mask,
                                            redop, reading, copy_mask,
                                            child_color, versions,
                                            preconditions, observed, 
                                            non_dominated);
          }
          else
          {
            // Otherwise do a quick test for non-interference on the
            // summary mask and iterate the users if needed
            if (!(copy_mask * event_users.user_mask))
            {
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                    it = event_users.users.multi_users->begin(); it !=
                    event_users.users.multi_users->end(); it++)
              {
                // You might think after we record one event dependence that
                // would be enough to skip the other users for the same event,
                // but we actually do need precise field information for each
                // event to properly issue dependent copies
                find_current_copy_preconditions(cit->first,it->first,it->second,
                                                redop, reading, copy_mask,
                                                child_color, versions,
                                                preconditions, observed,
                                                non_dominated);
              }
            }
          }
        }
        // See if we have any fields for which we need to do an analysis
        // on the previous fields
        // It's only safe to dominate fields that we observed
        dominated = (observed & (copy_mask - non_dominated));
        // Update the non-dominated mask with what we
        // we're actually not-dominated by
        non_dominated = copy_mask - dominated;
        const bool skip_analysis = !non_dominated;
        for (LegionMap<Event,EventUsers>::aligned::const_iterator pit = 
              previous_epoch_users.begin(); pit != 
              previous_epoch_users.end(); pit++)
        {
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE)
          // We're about to do a bunch of expensive tests, 
          // so first do something cheap to see if we can 
          // skip all the tests.
          if (pit->first.has_triggered())
          {
            dead_events.insert(pit->first);
            continue;
          }
#endif
          const EventUsers &event_users = pit->second;
          if (!!dominated)
          {
            FieldMask dom_overlap = event_users.user_mask & dominated;
            if (!!dom_overlap)
              filter_previous[pit->first] = dom_overlap;
          }
          // If we don't have any non-dominated fields we can skip the
          // rest of the analysis because we dominated everything
          if (skip_analysis)
            continue;
          if (event_users.single)
          {
            find_previous_copy_preconditions(pit->first,
                                             event_users.users.single_user,
                                             event_users.user_mask,
                                             redop, reading, non_dominated,
                                             child_color, versions,
                                             preconditions);
          }
          else
          {
            if (!(non_dominated * event_users.user_mask))
            {
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                    it = event_users.users.multi_users->begin(); it !=
                    event_users.users.multi_users->end(); it++)
              {
                find_previous_copy_preconditions(pit->first,
                                                 it->first, it->second,
                                                 redop, reading, 
                                                 non_dominated, child_color,
                                                 versions, preconditions);
              }
            }
          }
        }
      }
      // Release the lock, if we have any modifications to make, then
      // retake the lock in exclusive mode
      if (!dead_events.empty() || !filter_previous.empty() || !!dominated)
      {
        AutoLock v_lock(view_lock);
        if (!dead_events.empty())
        {
          for (std::set<Event>::const_iterator it = dead_events.begin();
                it != dead_events.end(); it++)
          {
            filter_local_users(*it);
          }
        }
        if (!filter_previous.empty())
          filter_previous_users(filter_previous);
        if (!!dominated)
          filter_current_users(dominated);
      }
    }

    //--------------------------------------------------------------------------
    void MaterializedView::find_current_copy_preconditions(Event test_event,
                                              const PhysicalUser *user,
                                              const FieldMask &user_mask,
                                              ReductionOpID redop, bool reading,
                                              const FieldMask &copy_mask,
                                              const ColorPoint &child_color,
                                              const FieldVersions *versions,
                             LegionMap<Event,FieldMask>::aligned &preconditions,
                                              FieldMask &observed,
                                              FieldMask &non_dominated)
    //--------------------------------------------------------------------------
    {
      FieldMask overlap = copy_mask & user_mask;
      if (!overlap)
        return;
      else
        observed |= overlap;
      if (child_color.is_valid())
      {
        // Same child, already done the analysis
        if (child_color == user->child)
        {
          non_dominated |= overlap;
          return;
        }
        // Disjoint children, keep going
        if (user->child.is_valid() &&
            logical_node->are_children_disjoint(child_color,
                                                user->child))
        {
          non_dominated |= overlap;
          return;
        }
      }
      // Now do a dependence analysis
      if (reading && IS_READ_ONLY(user->usage))
      {
        non_dominated |= overlap;
        return;
      }
      if ((redop > 0) && (user->usage.redop == redop))
      {
        non_dominated |= overlap;
        return;
      }
      // Check for WAR and WAW dependences, if we have one we
      // can see if we are writing the same version number
      // in which case there is no need for a dependence, thank
      // you wonchan and mini-aero for raising this case
      if (!reading && (redop == 0) && (versions != NULL) &&
          !IS_REDUCE(user->usage) && user->same_versions(overlap, versions))
      {
        non_dominated |= overlap;
        return;
      }
      // If we make it here, then we have a dependence, so record it 
      LegionMap<Event,FieldMask>::aligned::iterator finder = 
        preconditions.find(test_event);
      if (finder == preconditions.end())
        preconditions[test_event] = overlap;
      else
        finder->second |= overlap;
    }

    //--------------------------------------------------------------------------
    void MaterializedView::find_previous_copy_preconditions(Event test_event,
                                              const PhysicalUser *user,
                                              const FieldMask &user_mask,
                                              ReductionOpID redop, bool reading,
                                              const FieldMask &copy_mask,
                                              const ColorPoint &child_color,
                                              const FieldVersions *versions,
                        LegionMap<Event,FieldMask>::aligned &preconditions)
    //--------------------------------------------------------------------------
    { 
      if (child_color.is_valid())
      {
        // Same child: did analysis below
        if (child_color == user->child)
          return;
        if (user->child.is_valid() &&
            logical_node->are_children_disjoint(child_color,
                                                user->child))
          return;
      }
      FieldMask overlap = user_mask & copy_mask;
      if (!overlap)
        return;
      if (reading && IS_READ_ONLY(user->usage))
        return;
      if ((redop > 0) && (user->usage.redop == redop))
        return;
      if (!reading && (redop == 0) && (versions != NULL) &&
          !IS_REDUCE(user->usage) && user->same_versions(overlap, versions))
        return;
      // Otherwise record the dependence
      LegionMap<Event,FieldMask>::aligned::iterator finder = 
        preconditions.find(test_event);
      if (finder == preconditions.end())
        preconditions[test_event] = overlap;
      else
        finder->second |= overlap;
    }

    //--------------------------------------------------------------------------
    void MaterializedView::filter_previous_users(
                     const LegionMap<Event,FieldMask>::aligned &filter_previous)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<Event,FieldMask>::aligned::const_iterator fit = 
            filter_previous.begin(); fit != filter_previous.end(); fit++)
      {
        LegionMap<Event,EventUsers>::aligned::iterator finder = 
          previous_epoch_users.find(fit->first);
        // Someone might have already removed it
        if (finder == previous_epoch_users.end())
          continue;
        finder->second.user_mask -= fit->second;
        if (!finder->second.user_mask)
        {
          // We can delete the whole entry
          if (finder->second.single)
          {
            PhysicalUser *user = finder->second.users.single_user;
            if (user->remove_reference())
              legion_delete(user);
          }
          else
          {
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                  it = finder->second.users.multi_users->begin(); it !=
                  finder->second.users.multi_users->end(); it++)
            {
              if (it->first->remove_reference())
                legion_delete(it->first);
            }
            // Delete the map too
            delete finder->second.users.multi_users;
          }
          previous_epoch_users.erase(finder);
        }
        else if (!finder->second.single) // only need to filter for non-single
        {
          // Filter out the users for the dominated fields
          std::vector<PhysicalUser*> to_delete;
          for (LegionMap<PhysicalUser*,FieldMask>::aligned::iterator it = 
                finder->second.users.multi_users->begin(); it !=
                finder->second.users.multi_users->end(); it++)
          {
            it->second -= fit->second; 
            if (!it->second)
              to_delete.push_back(it->first);
          }
          if (!to_delete.empty())
          {
            for (std::vector<PhysicalUser*>::const_iterator it = 
                  to_delete.begin(); it != to_delete.end(); it++)
            {
              finder->second.users.multi_users->erase(*it);
              if ((*it)->remove_reference())
                legion_delete(*it);
            }
            // See if we can shrink this back down
            if (finder->second.users.multi_users->size() == 1)
            {
              LegionMap<PhysicalUser*,FieldMask>::aligned::iterator first_it =
                            finder->second.users.multi_users->begin();     
#ifdef DEBUG_HIGH_LEVEL
              // This summary mask should dominate
              assert(!(first_it->second - finder->second.user_mask));
#endif
              PhysicalUser *user = first_it->first;
              finder->second.user_mask = first_it->second;
              delete finder->second.users.multi_users;
              finder->second.users.single_user = user;
              finder->second.single = true;
            }
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void MaterializedView::filter_current_users(const FieldMask &dominated)
    //--------------------------------------------------------------------------
    {
      std::vector<Event> events_to_delete;
      for (LegionMap<Event,EventUsers>::aligned::iterator cit = 
            current_epoch_users.begin(); cit !=
            current_epoch_users.end(); cit++)
      {
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE)
        if (cit->first.has_triggered())
        {
          EventUsers &current_users = cit->second;
          if (current_users.single)
          {
            if (current_users.users.single_user->remove_reference())
              legion_delete(current_users.users.single_user);
          }
          else
          {
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::iterator it = 
                  current_users.users.multi_users->begin(); it !=
                  current_users.users.multi_users->end(); it++)
            {
              if (it->first->remove_reference())
                legion_delete(it->first);
            }
            delete current_users.users.multi_users;
          }
          events_to_delete.push_back(cit->first);
          continue;
        }
#endif
        EventUsers &current_users = cit->second;
        FieldMask summary_overlap = current_users.user_mask & dominated;
        if (!summary_overlap)
          continue;
        current_users.user_mask -= summary_overlap;
        EventUsers &prev_users = previous_epoch_users[cit->first];
        if (current_users.single)
        {
          PhysicalUser *user = current_users.users.single_user;
          if (prev_users.single)
          {
            // Single, see if something exists there yet
            if (prev_users.users.single_user == NULL)
            {
              prev_users.users.single_user = user; 
              prev_users.user_mask = summary_overlap;
              if (!current_users.user_mask) // reference flows back
                events_to_delete.push_back(cit->first); 
              else
                user->add_reference(); // add a reference
            }
            else if (prev_users.users.single_user == user)
            {
              // Same user, update the fields 
              prev_users.user_mask |= summary_overlap;
              if (!current_users.user_mask)
              {
                events_to_delete.push_back(cit->first);
                user->remove_reference(); // remove unnecessary reference
              }
            }
            else
            {
              // Go to multi mode
              LegionMap<PhysicalUser*,FieldMask>::aligned *new_map = 
                            new LegionMap<PhysicalUser*,FieldMask>::aligned();
              (*new_map)[prev_users.users.single_user] = prev_users.user_mask;
              (*new_map)[user] = summary_overlap;
              if (!current_users.user_mask) // reference flows back
                events_to_delete.push_back(cit->first); 
              else
                user->add_reference();
              prev_users.user_mask |= summary_overlap;
              prev_users.users.multi_users = new_map;
              prev_users.single = false;
            }
          }
          else
          {
            // Already multi
            prev_users.user_mask |= summary_overlap;
            // See if we can find it in the multi-set
            LegionMap<PhysicalUser*,FieldMask>::aligned::iterator finder = 
              prev_users.users.multi_users->find(user);
            if (finder == prev_users.users.multi_users->end())
            {
              // Couldn't find it
              (*prev_users.users.multi_users)[user] = summary_overlap;
              if (!current_users.user_mask) // reference flows back
                events_to_delete.push_back(cit->first); 
              else
                user->add_reference();
            }
            else
            {
              // Found it, update it 
              finder->second |= summary_overlap;
              if (!current_users.user_mask)
              {
                events_to_delete.push_back(cit->first);
                user->remove_reference(); // remove redundant reference
              }
            }
          }
        }
        else
        {
          // Many things, filter them and move them back
          if (!current_users.user_mask)
          {
            // Moving the whole set back, see what the previous looks like
            if (prev_users.single)
            {
              if (prev_users.users.single_user != NULL)
              {
                // Merge the one user into this map so we can move 
                // the whole map back
                PhysicalUser *user = prev_users.users.single_user;  
                LegionMap<PhysicalUser*,FieldMask>::aligned::iterator finder =
                  current_users.users.multi_users->find(user);
                if (finder == current_users.users.multi_users->end())
                {
                  // Add it reference is already there
                  (*current_users.users.multi_users)[user] = 
                    prev_users.user_mask;
                }
                else
                {
                  // Already there, update it and remove duplicate reference
                  finder->second |= prev_users.user_mask;
                  user->remove_reference();
                }
              }
              // Now just move the map back
              prev_users.user_mask |= summary_overlap;
              prev_users.users.multi_users = current_users.users.multi_users;
              prev_users.single = false;
            }
            else
            {
              // merge the two sets
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator
                    it = current_users.users.multi_users->begin();
                    it != current_users.users.multi_users->end(); it++)
              {
                // See if we can find it
                LegionMap<PhysicalUser*,FieldMask>::aligned::iterator finder = 
                  prev_users.users.multi_users->find(it->first);
                if (finder == prev_users.users.multi_users->end())
                {
                  // Didn't find it, just move it back, reference moves back
                  prev_users.users.multi_users->insert(*it);
                }
                else
                {
                  finder->second |= it->second; 
                  // Remove the duplicate reference
                  it->first->remove_reference();
                }
              }
              prev_users.user_mask |= summary_overlap;
              // Now delete the set
              delete current_users.users.multi_users;
            }
            events_to_delete.push_back(cit->first);
          }
          else
          {
            // Only send back filtered users
            std::vector<PhysicalUser*> to_delete;
            if (prev_users.single)
            {
              // Make a new map to send back  
              LegionMap<PhysicalUser*,FieldMask>::aligned *new_map = 
                            new LegionMap<PhysicalUser*,FieldMask>::aligned();
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::iterator it = 
                    current_users.users.multi_users->begin(); it !=
                    current_users.users.multi_users->end(); it++)
              {
                FieldMask overlap = summary_overlap & it->second;
                if (!overlap)
                  continue;
                // Can move without checking
                (*new_map)[it->first] = overlap;
                it->second -= overlap;
                if (!it->second)
                  to_delete.push_back(it->first); // reference flows back
                else
                  it->first->add_reference(); // need new reference
              }
              // Also capture the existing previous user if there is one
              if (prev_users.users.single_user != NULL)
              {
                LegionMap<PhysicalUser*,FieldMask>::aligned::iterator finder = 
                  new_map->find(prev_users.users.single_user);
                if (finder == new_map->end())
                {
                  (*new_map)[prev_users.users.single_user] = 
                    prev_users.user_mask;
                }
                else
                {
                  finder->second |= prev_users.user_mask;
                  // Remove redundant reference
                  finder->first->remove_reference();
                }
              }
              // Make the new map the previous set
              prev_users.user_mask |= summary_overlap;
              prev_users.users.multi_users = new_map;
              prev_users.single = false;
            }
            else
            {
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::iterator it =
                    current_users.users.multi_users->begin(); it !=
                    current_users.users.multi_users->end(); it++)
              {
                FieldMask overlap = summary_overlap & it->second; 
                if (!overlap)
                  continue;
                it->second -= overlap;
                LegionMap<PhysicalUser*,FieldMask>::aligned::iterator finder = 
                  prev_users.users.multi_users->find(it->first);
                // See if it already exists
                if (finder == prev_users.users.multi_users->end())
                {
                  // Doesn't exist yet, so add it 
                  (*prev_users.users.multi_users)[it->first] = overlap;
                  if (!it->second) // reference flows back
                    to_delete.push_back(it->first);
                  else
                    it->first->add_reference();
                }
                else
                {
                  // Already exists so update it
                  finder->second |= overlap;
                  if (!it->second)
                  {
                    to_delete.push_back(it->first);
                    // Remove redundant reference
                    it->first->remove_reference();
                  }
                }
              }
              prev_users.user_mask |= summary_overlap;
            }
            // See if we can collapse this map back down
            if (!to_delete.empty())
            {
              for (std::vector<PhysicalUser*>::const_iterator it = 
                    to_delete.begin(); it != to_delete.end(); it++)
              {
                current_users.users.multi_users->erase(*it);
              }
              if (current_users.users.multi_users->size() == 1)
              {
                LegionMap<PhysicalUser*,FieldMask>::aligned::iterator 
                  first_it = current_users.users.multi_users->begin();
#ifdef DEBUG_HIGH_LEVEL
                // Should dominate as an upper bound
                assert(!(first_it->second - current_users.user_mask));
#endif
                PhysicalUser *user = first_it->first;
                current_users.user_mask = first_it->second;
                delete current_users.users.multi_users;
                current_users.users.single_user = user;   
                current_users.single = true;
              }
            }
          }
        }
      }
      // Delete any events
      if (!events_to_delete.empty())
      {
        for (std::vector<Event>::const_iterator it = events_to_delete.begin();
              it != events_to_delete.end(); it++)
        {
          current_epoch_users.erase(*it); 
        }
      }
    }

    //--------------------------------------------------------------------------
    void MaterializedView::add_current_user(PhysicalUser *user, 
                                            Event term_event,
                                            const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      // Reference should already have been added
      EventUsers &event_users = current_epoch_users[term_event];
      if (event_users.single)
      {
        if (event_users.users.single_user == NULL)
        {
          // make it the entry
          event_users.users.single_user = user;
          event_users.user_mask = user_mask;
        }
        else
        {
          // convert to multi
          LegionMap<PhysicalUser*,FieldMask>::aligned *new_map = 
                           new LegionMap<PhysicalUser*,FieldMask>::aligned();
          (*new_map)[event_users.users.single_user] = event_users.user_mask;
          (*new_map)[user] = user_mask;
          event_users.user_mask |= user_mask;
          event_users.users.multi_users = new_map;
          event_users.single = false;
        }
      }
      else
      {
        // Add it to the set 
        (*event_users.users.multi_users)[user] = user_mask;
        event_users.user_mask |= user_mask;
      }
    }

    //--------------------------------------------------------------------------
    void MaterializedView::add_previous_user(PhysicalUser *user, 
                                             Event term_event,
                                             const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      // Reference should already have been added
      EventUsers &event_users = previous_epoch_users[term_event];
      if (event_users.single)
      {
        if (event_users.users.single_user == NULL)
        {
          // make it the entry
          event_users.users.single_user = user;
          event_users.user_mask = user_mask;
        }
        else
        {
          // convert to multi
          LegionMap<PhysicalUser*,FieldMask>::aligned *new_map = 
                           new LegionMap<PhysicalUser*,FieldMask>::aligned();
          (*new_map)[event_users.users.single_user] = event_users.user_mask;
          (*new_map)[user] = user_mask;
          event_users.user_mask |= user_mask;
          event_users.users.multi_users = new_map;
          event_users.single = false;
        }
      }
      else
      {
        // Add it to the set 
        (*event_users.users.multi_users)[user] = user_mask;
        event_users.user_mask |= user_mask;
      }
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::has_war_dependence_above(const RegionUsage &usage,
                                                    const FieldMask &user_mask,
                                                  const ColorPoint &child_color)
    //--------------------------------------------------------------------------
    {
      const ColorPoint &local_color = logical_node->get_color();
      if (has_local_war_dependence(usage, user_mask, child_color, local_color))
        return true;
      if (parent != NULL)
        return parent->has_war_dependence_above(usage, user_mask, local_color);
      return false;
    }

    //--------------------------------------------------------------------------
    bool MaterializedView::has_local_war_dependence(const RegionUsage &usage,
                                                    const FieldMask &user_mask,
                                                  const ColorPoint &child_color,
                                                  const ColorPoint &local_color)
    //--------------------------------------------------------------------------
    {
      // Do the local analysis
      FieldMask observed;
      AutoLock v_lock(view_lock,1,false/*exclusive*/);
      for (LegionMap<Event,EventUsers>::aligned::const_iterator cit = 
            current_epoch_users.begin(); cit != 
            current_epoch_users.end(); cit++)
      {
        const EventUsers &event_users = cit->second;
        FieldMask overlap = user_mask & event_users.user_mask;
        if (!overlap)
          continue;
        else
          observed |= overlap;
        if (event_users.single)
        {
          if (IS_READ_ONLY(event_users.users.single_user->usage))
            return true;
        }
        else
        {
          for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator it =
                event_users.users.multi_users->begin(); it !=
                event_users.users.multi_users->end(); it++)
          {
            FieldMask overlap2 = user_mask & it->second;
            if (!overlap2)
              continue;
            if (IS_READ_ONLY(it->first->usage))
              return true;
          }
        }
      }
      FieldMask not_observed = user_mask - observed;
      // If we had fields that were not observed, check the previous list
      if (!!not_observed)
      {
        for (LegionMap<Event,EventUsers>::aligned::const_iterator pit = 
              previous_epoch_users.begin(); pit != 
              previous_epoch_users.end(); pit++)
        {
          const EventUsers &event_users = pit->second;
          if (event_users.single)
          {
            FieldMask overlap = user_mask & event_users.user_mask;
            if (!overlap)
              continue;
            if (IS_READ_ONLY(event_users.users.single_user->usage))
              return true;
          }
          else
          {
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                  it = event_users.users.multi_users->begin(); it !=
                  event_users.users.multi_users->end(); it++)
            {
              FieldMask overlap = user_mask & it->second;
              if (!overlap)
                continue;
              if (IS_READ_ONLY(it->first->usage))
                return true;
            }
          }
        }
      }
      return false;
    }
    
#if 0
    //--------------------------------------------------------------------------
    void MaterializedView::update_versions(const FieldMask &update_mask)
    //--------------------------------------------------------------------------
    {
      std::vector<VersionID> to_delete;
      LegionMap<VersionID,FieldMask>::aligned new_versions;
      for (LegionMap<VersionID,FieldMask>::aligned::iterator it = 
            current_versions.begin(); it != current_versions.end(); it++)
      {
        FieldMask overlap = it->second & update_mask;
        if (!!overlap)
        {
          new_versions[(it->first+1)] = overlap; 
          it->second -= update_mask;
          if (!it->second)
            to_delete.push_back(it->first);
        }
      }
      for (std::vector<VersionID>::const_iterator it = to_delete.begin();
            it != to_delete.end(); it++)
      {
        current_versions.erase(*it);
      }
      for (LegionMap<VersionID,FieldMask>::aligned::const_iterator it = 
            new_versions.begin(); it != new_versions.end(); it++)
      {
        LegionMap<VersionID,FieldMask>::aligned::iterator finder = 
          current_versions.find(it->first);
        if (finder == current_versions.end())
          current_versions.insert(*it);
        else
          finder->second |= it->second;
      }
    }
#endif

    //--------------------------------------------------------------------------
    void MaterializedView::filter_local_users(Event term_event) 
    //--------------------------------------------------------------------------
    {
      // Don't do this if we are in Legion Spy since we want to see
      // all of the dependences on an instance
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE)
      std::set<Event>::iterator event_finder = 
        outstanding_gc_events.find(term_event); 
      if (event_finder != outstanding_gc_events.end())
      {
        LegionMap<Event,EventUsers>::aligned::iterator current_finder = 
          current_epoch_users.find(term_event);
        if (current_finder != current_epoch_users.end())
        {
          EventUsers &event_users = current_finder->second;
          if (event_users.single)
          {
            if (event_users.users.single_user->remove_reference())
              legion_delete(event_users.users.single_user);
          }
          else
          {
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::iterator
                  it = event_users.users.multi_users->begin(); it !=
                  event_users.users.multi_users->end(); it++)
            {
              if (it->first->remove_reference())
                legion_delete(it->first);
            }
            delete event_users.users.multi_users;
          }
          current_epoch_users.erase(current_finder);
        }
        LegionMap<Event,EventUsers>::aligned::iterator previous_finder = 
          previous_epoch_users.find(term_event);
        if (previous_finder != previous_epoch_users.end())
        {
          EventUsers &event_users = previous_finder->second; 
          if (event_users.single)
          {
            if (event_users.users.single_user->remove_reference())
              legion_delete(event_users.users.single_user);
          }
          else
          {
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::iterator
                  it = event_users.users.multi_users->begin(); it !=
                  event_users.users.multi_users->end(); it++)
            {
              if (it->first->remove_reference())
                legion_delete(it->first);
            }
            delete event_users.users.multi_users;
          }
          previous_epoch_users.erase(previous_finder);
        }
        outstanding_gc_events.erase(event_finder);
      }
#endif
    }

    //--------------------------------------------------------------------------
    void MaterializedView::find_atomic_reservations(const FieldMask &mask,
                                                    Operation *op, bool excl)
    //--------------------------------------------------------------------------
    {
      // Keep going up the tree until we get to the root
      if (parent == NULL)
      {
        // Compute the field set
        std::vector<FieldID> atomic_fields;
        logical_node->column_source->get_field_set(mask, atomic_fields);
        // If we are the owner we can do this here
        if (is_owner())
        {
          std::vector<Reservation> reservations(atomic_fields.size());
          find_field_reservations(atomic_fields, reservations);
          for (unsigned idx = 0; idx < reservations.size(); idx++)
            op->update_atomic_locks(reservations[idx], excl);
        }
        else
        {
          // Figure out which fields we need requests for and send them
          std::vector<FieldID> needed_fields;
          {
            AutoLock v_lock(view_lock, 1, false);
            for (std::vector<FieldID>::const_iterator it = 
                  atomic_fields.begin(); it != atomic_fields.end(); it++)
            {
              std::map<FieldID,Reservation>::const_iterator finder = 
                atomic_reservations.find(*it);
              if (finder == atomic_reservations.end())
                needed_fields.push_back(*it);
              else
                op->update_atomic_locks(finder->second, excl);
            }
          }
          if (!needed_fields.empty())
          {
            UserEvent wait_on = UserEvent::create_user_event();
            Serializer rez;
            {
              RezCheck z(rez);
              rez.serialize(did);
              rez.serialize<size_t>(needed_fields.size());
              for (unsigned idx = 0; idx < needed_fields.size(); idx++)
                rez.serialize(needed_fields[idx]);
              rez.serialize(wait_on);
            }
            runtime->send_atomic_reservation_request(owner_space, rez);
            wait_on.wait();
            // Now retake the lock and get the remaining reservations
            AutoLock v_lock(view_lock, 1, false);
            for (std::vector<FieldID>::const_iterator it = 
                  needed_fields.begin(); it != needed_fields.end(); it++)
            {
              std::map<FieldID,Reservation>::const_iterator finder =
                atomic_reservations.find(*it);
#ifdef DEBUG_HIGH_LEVEL
              assert(finder != atomic_reservations.end());
#endif
              op->update_atomic_locks(finder->second, excl);
            }
          }
        }
      }
      else
        parent->find_atomic_reservations(mask, op, excl);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::set_descriptor(FieldDataDescriptor &desc,
                                          FieldID field_id) const
    //--------------------------------------------------------------------------
    {
      // Get the low-level index space
      const Domain &dom = logical_node->get_domain_no_wait();
      desc.index_space = dom.get_index_space();
      // Then ask the manager to fill in the rest of the information
      manager->set_descriptor(desc, field_id);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::find_field_reservations(
                                    const std::vector<FieldID> &needed_fields, 
                                    std::vector<Reservation> &results)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(is_owner());
      assert(needed_fields.size() == results.size());
#endif
      AutoLock v_lock(view_lock);
      for (unsigned idx = 0; idx < needed_fields.size(); idx++)
      {
        std::map<FieldID,Reservation>::const_iterator finder = 
          atomic_reservations.find(needed_fields[idx]);
        if (finder == atomic_reservations.end())
        {
          // Make a new reservation and add it to the set
          Reservation handle = Reservation::create_reservation();
          atomic_reservations[needed_fields[idx]] = handle;
          results[idx] = handle;
        }
        else
          results[idx] = finder->second;
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void MaterializedView::handle_send_atomic_reservation_request(
                   Runtime *runtime, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      size_t num_fields;
      derez.deserialize(num_fields);
      std::vector<FieldID> fields(num_fields);
      for (unsigned idx = 0; idx < num_fields; idx++)
        derez.deserialize(fields[idx]);
      UserEvent to_trigger;
      derez.deserialize(to_trigger);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_HIGH_LEVEL
      MaterializedView *target = dynamic_cast<MaterializedView*>(dc);
      assert(target != NULL);
#else
      MaterializedView *target = static_cast<MaterializedView*>(dc);
#endif
      std::vector<Reservation> reservations(num_fields);
      target->find_field_reservations(fields, reservations);
      Serializer rez;
      {
        RezCheck z2(rez);
        rez.serialize(did);
        rez.serialize(num_fields);
        for (unsigned idx = 0; idx < num_fields; idx++)
        {
          rez.serialize(fields[idx]);
          rez.serialize(reservations[idx]);
        }
        rez.serialize(to_trigger);
      }
      runtime->send_atomic_reservation_response(source, rez);
    }

    //--------------------------------------------------------------------------
    void MaterializedView::update_field_reservations(
                                  const std::vector<FieldID> &fields, 
                                  const std::vector<Reservation> &reservations)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(!is_owner());
      assert(fields.size() == reservations.size());
#endif
      AutoLock v_lock(view_lock);
      for (unsigned idx = 0; idx < fields.size(); idx++)
        atomic_reservations[fields[idx]] = reservations[idx];
    }

    //--------------------------------------------------------------------------
    /*static*/ void MaterializedView::handle_send_atomic_reservation_response(
                                          Runtime *runtime, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      size_t num_fields;
      derez.deserialize(num_fields);
      std::vector<FieldID> fields(num_fields);
      std::vector<Reservation> reservations(num_fields);
      for (unsigned idx = 0; idx < num_fields; idx++)
      {
        derez.deserialize(fields[idx]);
        derez.deserialize(reservations[idx]);
      }
      UserEvent to_trigger;
      derez.deserialize(to_trigger);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_HIGH_LEVEL
      MaterializedView *target = dynamic_cast<MaterializedView*>(dc);
      assert(target != NULL);
#else
      MaterializedView *target = static_cast<MaterializedView*>(dc);
#endif
      target->update_field_reservations(fields, reservations);
      to_trigger.trigger();
    }

    //--------------------------------------------------------------------------
    /*static*/ void MaterializedView::handle_send_materialized_view(
                  Runtime *runtime, Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez); 
      DistributedID did;
      derez.deserialize(did);
      DistributedID manager_did;
      derez.deserialize(manager_did);
      DistributedID parent_did;
      derez.deserialize(parent_did);
      LogicalRegion handle;
      derez.deserialize(handle);
      AddressSpaceID owner_space;
      derez.deserialize(owner_space);
      UniqueID context_uid;
      derez.deserialize(context_uid);

      RegionNode *target_node = runtime->forest->get_node(handle); 
      Event man_ready = Event::NO_EVENT;
      PhysicalManager *phy_man = 
        runtime->find_or_request_physical_manager(manager_did, man_ready);
      MaterializedView *parent = NULL;
      if (parent_did != 0)
      {
        Event par_ready = Event::NO_EVENT;
        LogicalView *par_view = 
          runtime->find_or_request_logical_view(parent_did, par_ready);
        if (par_ready.exists())
          par_ready.wait();
#ifdef DEBUG_HIGH_LEVEL
        assert(par_view->is_materialized_view());
#endif
        parent = par_view->as_materialized_view();
      }
      if (man_ready.exists())
        man_ready.wait();
#ifdef DEBUG_HIGH_LEVEL
      assert(phy_man->is_instance_manager());
#endif
      InstanceManager *inst_manager = phy_man->as_instance_manager();
      SingleTask *owner_context = runtime->find_context(context_uid);
      void *location;
      if (runtime->find_pending_collectable_location(did, location))
        legion_new_in_place<MaterializedView>(location, runtime->forest,
                                              did, owner_space, 
                                              runtime->address_space,
                                              target_node, inst_manager,
                                              parent, owner_context);
      else
        legion_new<MaterializedView>(runtime->forest, did, owner_space,
                                     runtime->address_space, target_node,
                                     inst_manager, parent, owner_context);
    }

    //--------------------------------------------------------------------------
    /*static*/ void MaterializedView::handle_send_update(Runtime *runtime,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez); 
      bool is_region;
      derez.deserialize(is_region);
      RegionTreeNode *target_node;
      if (is_region)
      {
        LogicalRegion handle;
        derez.deserialize(handle);
        target_node = runtime->forest->get_node(handle);
      }
      else
      {
        LogicalPartition handle;
        derez.deserialize(handle);
        target_node = runtime->forest->get_node(handle);
      }
      DistributedID did;
      derez.deserialize(did);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_HIGH_LEVEL
      MaterializedView *mat_view = dynamic_cast<MaterializedView*>(dc);
      assert(mat_view != NULL);
#else
      MaterializedView *mat_view = static_cast<MaterializedView*>(dc);
#endif
      mat_view->process_update(derez, source);
    }

    /////////////////////////////////////////////////////////////
    // DeferredView 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    DeferredView::DeferredView(RegionTreeForest *ctx, DistributedID did,
                               AddressSpaceID owner_sp, AddressSpaceID local_sp,
                               RegionTreeNode *node)
      : LogicalView(ctx, did, owner_sp, local_sp, node)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    DeferredView::~DeferredView(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    void DeferredView::issue_deferred_copies(const TraversalInfo &info,
                                              MaterializedView *dst,
                                              const FieldMask &copy_mask,
                                              CopyTracker *tracker)
    //--------------------------------------------------------------------------
    {
      // Find the destination preconditions first 
      LegionMap<Event,FieldMask>::aligned preconditions;
      dst->find_copy_preconditions(0/*redop*/, false/*reading*/,
                                   copy_mask, info.version_info, preconditions);
      LegionMap<Event,FieldMask>::aligned postconditions;
      issue_deferred_copies(info, dst, copy_mask, preconditions, 
                            postconditions, tracker);
      // Register the resulting events as users of the destination
      for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
            postconditions.begin(); it != postconditions.end(); it++)
      {
        dst->add_copy_user(0/*redop*/, it->first, info.version_info, 
                           it->second, false/*reading*/);
      }
    }

    //--------------------------------------------------------------------------
    void DeferredView::issue_deferred_copies_across(const TraversalInfo &info,
                                                     MaterializedView *dst,
                                      const std::vector<unsigned> &src_indexes,
                                      const std::vector<unsigned> &dst_indexes,
                                                     Event precondition,
                                               std::set<Event> &postconditions)
    //--------------------------------------------------------------------------
    {
      bool perfect = true;
      FieldMask src_mask, dst_mask;
      for (unsigned idx = 0; idx < dst_indexes.size(); idx++)
      {
        src_mask.set_bit(src_indexes[idx]);
        dst_mask.set_bit(dst_indexes[idx]);
        if (perfect && (src_indexes[idx] != dst_indexes[idx]))
          perfect = false;
      }
      // Initialize the preconditions
      LegionMap<Event,FieldMask>::aligned preconditions;
      preconditions[precondition] = src_mask;
      LegionMap<Event,FieldMask>::aligned local_postconditions;
      // A seemingly common case but not the general one, if the fields
      // are in the same locations for the source and destination then
      // we can just do the normal deferred copy routine
      if (perfect)
      {
        issue_deferred_copies(info, dst, src_mask, preconditions, 
                              local_postconditions);
      }
      else
      {
        // Initialize the across copy helper
        CopyAcrossHelper across_helper(src_mask);
        dst->manager->initialize_across_helper(&across_helper, dst_mask, 
                                               src_indexes, dst_indexes);
        issue_deferred_copies(info, dst, src_mask, preconditions, 
                              local_postconditions, NULL, &across_helper);
      }
      // Put the local postconditions in the result
      for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
           local_postconditions.begin(); it != local_postconditions.end(); it++)
      {
        postconditions.insert(it->first);
      }
    }

    //--------------------------------------------------------------------------
    void DeferredView::find_field_descriptors(Event term_event,
                                          const RegionUsage &usage,
                                          const FieldMask &user_mask,
                                          FieldID field_id, Operation *op,
                                  std::vector<FieldDataDescriptor> &field_data,
                                          std::set<Event> &preconditions)
    //--------------------------------------------------------------------------
    {
      // TODO: reimplement this for dependent partitioning
      assert(false);
    }

    /////////////////////////////////////////////////////////////
    // CompositeNode 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    CompositeVersionInfo::CompositeVersionInfo(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    CompositeVersionInfo::CompositeVersionInfo(const CompositeVersionInfo &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    CompositeVersionInfo::~CompositeVersionInfo(void)
    //--------------------------------------------------------------------------
    {
      version_info.release();
    }

    //--------------------------------------------------------------------------
    CompositeVersionInfo& CompositeVersionInfo::operator=(
                                                const CompositeVersionInfo &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    /////////////////////////////////////////////////////////////
    // CompositeView
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    CompositeView::CompositeView(RegionTreeForest *ctx, DistributedID did,
                              AddressSpaceID owner_proc, RegionTreeNode *node,
                              AddressSpaceID local_proc, CompositeNode *r,
                              CompositeVersionInfo *info)
      : DeferredView(ctx, encode_composite_did(did), 
                     owner_proc, local_proc, node),
        root(r), version_info(info) 
    {
      version_info->add_reference();
      root->set_owner_did(did);
      // Do remote registration if necessary
      if (!is_owner())
      {
        add_base_resource_ref(REMOTE_DID_REF);
        send_remote_registration();
      }
#ifdef LEGION_GC
      log_garbage.info("GC Composite View %ld", did);
#endif
    }

    //--------------------------------------------------------------------------
    CompositeView::CompositeView(const CompositeView &rhs)
      : DeferredView(NULL, 0, 0, 0, NULL), root(NULL), version_info(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    CompositeView::~CompositeView(void)
    //--------------------------------------------------------------------------
    {
      if (is_owner())
      {
        UpdateReferenceFunctor<RESOURCE_REF_KIND,false/*add*/> functor(this);
        map_over_remote_instances(functor);
      }
      // Delete our root
      legion_delete(root);
      // See if we can delete our version info
      if (version_info->remove_reference())
        delete version_info;
    }

    //--------------------------------------------------------------------------
    CompositeView& CompositeView::operator=(const CompositeView &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* CompositeView::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<CompositeView,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void CompositeView::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    void CompositeView::notify_active(void)
    //--------------------------------------------------------------------------
    {
      root->notify_active();
    }

    //--------------------------------------------------------------------------
    void CompositeView::notify_inactive(void)
    //--------------------------------------------------------------------------
    {
      root->notify_inactive(); 
    }

    //--------------------------------------------------------------------------
    void CompositeView::notify_valid(void)
    //--------------------------------------------------------------------------
    {
      root->notify_valid();
    }

    //--------------------------------------------------------------------------
    void CompositeView::notify_invalid(void)
    //--------------------------------------------------------------------------
    {
      root->notify_invalid();
    }

    //--------------------------------------------------------------------------
    void CompositeView::send_view(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      // Don't take the lock, it's alright to have duplicate sends
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(did);
        rez.serialize(owner_space);
        bool is_region = logical_node->is_region();
        rez.serialize(is_region);
        if (is_region)
          rez.serialize(logical_node->as_region_node()->handle);
        else
          rez.serialize(logical_node->as_partition_node()->handle);
        VersionInfo &info = version_info->get_version_info();
        info.pack_version_info(rez, 0, 0);
        root->pack_composite_tree(rez, target);
      }
      runtime->send_composite_view(target, rez);
      update_remote_instances(target);
    }

    //--------------------------------------------------------------------------
    void CompositeView::send_view_updates(AddressSpaceID target, 
                                          const FieldMask &update_mask)
    //--------------------------------------------------------------------------
    {
      // Do nothing, composite instances never have updates
    }

    //--------------------------------------------------------------------------
    void CompositeView::make_local(std::set<Event> &preconditions)
    //--------------------------------------------------------------------------
    {
      VersionInfo &info = version_info->get_version_info();
      info.make_local(preconditions, context, 0/*dummy ctx*/);
      std::set<DistributedID> checked_views;
      root->make_local(preconditions, checked_views);
    }

    //--------------------------------------------------------------------------
    LogicalView* CompositeView::get_subview(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // Composite views don't need subviews
      return this;
    }

    //--------------------------------------------------------------------------
    DeferredView* CompositeView::simplify(CompositeCloser &closer,
                                          const FieldMask &capture_mask)
    //-------------------------------------------------------------------------
    {
      CompositeNode *new_root = legion_new<CompositeNode>(logical_node, 
                                                          (CompositeNode*)NULL);
      FieldMask captured_mask = capture_mask;
      if (root->simplify(closer, captured_mask, new_root))
      {
        DistributedID new_did = 
          context->runtime->get_available_distributed_id(false);
        // TODO: simplify the version info here too
        // to avoid moving around extra state
        // Make a new composite view
        return legion_new<CompositeView>(context, new_did, 
            context->runtime->address_space, logical_node, 
            context->runtime->address_space, new_root, 
            version_info);
      }
      else // didn't change so we can delete the new root and return ourself
      {
        legion_delete(new_root);
        return this;
      }
    }

    //--------------------------------------------------------------------------
    void CompositeView::issue_deferred_copies(const TraversalInfo &info,
                                              MaterializedView *dst,
                                              const FieldMask &copy_mask,
                      const LegionMap<Event,FieldMask>::aligned &preconditions,
                            LegionMap<Event,FieldMask>::aligned &postconditions,
                                              CopyTracker *tracker,
                                              CopyAcrossHelper *across_helper)
    //--------------------------------------------------------------------------
    {
      LegionMap<Event,FieldMask>::aligned postreductions;
      root->issue_deferred_copies(info, dst, copy_mask, 
                                  version_info->get_version_info(), 
                                  preconditions, postconditions, 
                                  postreductions, tracker, across_helper);
      if (!postreductions.empty())
      {
        // We need to merge the two post sets
        postreductions.insert(postconditions.begin(), postconditions.end());
        // Clear this out since this is where we will put the results
        postconditions.clear();
        // Now sort them and reduce them
        LegionList<EventSet>::aligned event_sets; 
        RegionTreeNode::compute_event_sets(copy_mask, 
                                           postreductions, event_sets);
        for (LegionList<EventSet>::aligned::const_iterator it = 
              event_sets.begin(); it != event_sets.end(); it++)
        {
          if (it->preconditions.size() == 1)
          {
            Event post = *(it->preconditions.begin());
            if (!post.exists())
              continue;
            postconditions[post] = it->set_mask;
          }
          else
          {
            Event post = Runtime::merge_events<false>(it->preconditions);
            if (!post.exists())
              continue;
            postconditions[post] = it->set_mask;
          }
        }
      }
    } 

    //--------------------------------------------------------------------------
    /*static*/ void CompositeView::handle_send_composite_view(Runtime *runtime,
                                    Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez); 
      DistributedID did;
      derez.deserialize(did);
      AddressSpaceID owner;
      derez.deserialize(owner);
      bool is_region;
      derez.deserialize(is_region);
      RegionTreeNode *target_node;
      if (is_region)
      {
        LogicalRegion handle;
        derez.deserialize(handle);
        target_node = runtime->forest->get_node(handle);
      }
      else
      {
        LogicalPartition handle;
        derez.deserialize(handle);
        target_node = runtime->forest->get_node(handle);
      }
      CompositeVersionInfo *version_info = new CompositeVersionInfo();
      VersionInfo &info = version_info->get_version_info();
      info.unpack_version_info(derez);
      CompositeNode *root = legion_new<CompositeNode>(target_node, 
                                                      (CompositeNode*)NULL);
      std::set<Event> ready_events;
      std::map<LogicalView*,unsigned> pending_refs;
      root->unpack_composite_tree(derez, source, runtime,
                                  ready_events, pending_refs);
      // If we have anything to wait for do that now
      if (!ready_events.empty())
      {
        Event wait_on = Runtime::merge_events<true>(ready_events);
        wait_on.wait();
      }
      if (!pending_refs.empty())
      {
        // Add any resource refs for views that were not ready until now
        for (std::map<LogicalView*,unsigned>::const_iterator it = 
              pending_refs.begin(); it != pending_refs.end(); it++)
        {
          it->first->add_base_resource_ref(COMPOSITE_NODE_REF, it->second);
        }
      }
      void *location;
      if (runtime->find_pending_collectable_location(did, location))
        legion_new_in_place<CompositeView>(location, runtime->forest, did,
                                           owner, target_node, 
                                           runtime->address_space,
                                           root, version_info);
      else
        legion_new<CompositeView>(runtime->forest, did, owner, target_node, 
                                  runtime->address_space, root, version_info);
    }

    /////////////////////////////////////////////////////////////
    // CompositeNode 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    CompositeNode::CompositeNode(RegionTreeNode* node, CompositeNode *p)
      : logical_node(node), parent(p), owner_did(0)
    //--------------------------------------------------------------------------
    {
      if (parent != NULL)
        parent->add_child(this);
    }

    //--------------------------------------------------------------------------
    CompositeNode::CompositeNode(const CompositeNode &rhs)
      : logical_node(NULL), parent(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    CompositeNode::~CompositeNode(void)
    //--------------------------------------------------------------------------
    {
      // Free up all our children 
      for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        legion_delete(it->first);
      }
      // Remove our resource references
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it =
            valid_views.begin(); it != valid_views.end(); it++)
      {
        if (it->first->remove_base_resource_ref(COMPOSITE_NODE_REF))
          LogicalView::delete_logical_view(it->first);
      }
      valid_views.clear();
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it =
            reduction_views.begin(); it != reduction_views.end(); it++)
      {
        if (it->first->remove_base_resource_ref(COMPOSITE_NODE_REF))
          legion_delete(it->first);
      }
      reduction_views.clear();
    }

    //--------------------------------------------------------------------------
    CompositeNode& CompositeNode::operator=(const CompositeNode &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* CompositeNode::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<CompositeNode,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void CompositeNode::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    void CompositeNode::add_child(CompositeNode *child)
    //--------------------------------------------------------------------------
    {
      // Referencing it should instantiate it
      children[child];
    }

    //--------------------------------------------------------------------------
    void CompositeNode::update_child(CompositeNode *child,const FieldMask &mask)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(children.find(child) != children.end());
#endif
      children[child] |= mask;
    }

    //--------------------------------------------------------------------------
    void CompositeNode::finalize(FieldMask &final_mask)
    //--------------------------------------------------------------------------
    {
      if (!children.empty())
      {
        for (LegionMap<CompositeNode*,FieldMask>::aligned::iterator it =
              children.begin(); it != children.end(); it++)
        {
          it->first->finalize(it->second);
          final_mask |= it->second;
        }
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::set_owner_did(DistributedID own_did)
    //--------------------------------------------------------------------------
    {
      owner_did = own_did;
      for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        it->first->set_owner_did(own_did);
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::capture_physical_state(CompositeCloser &closer,
                                               PhysicalState *state,
                                               const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      // Check to see if this is the root, if it is, we need to pull
      // the valid instance views from the state
      if (parent == NULL)
      {
        LegionMap<LogicalView*,FieldMask>::aligned instances;
        logical_node->find_valid_instance_views(closer.ctx, state, capture_mask,
            capture_mask, closer.version_info, false/*needs space*/, instances);
        capture_instances(closer, capture_mask, &instances);
      }
      else
      {
        // Tell the parent about our capture
        parent->update_child(this, capture_mask);
        if (!!state->dirty_mask & !state->valid_views.empty())
        {
          dirty_mask = state->dirty_mask & capture_mask;
          if (!!dirty_mask)
          {
            // C++ sucks sometimes
            LegionMap<LogicalView*,FieldMask>::aligned *valid_views = 
              reinterpret_cast<LegionMap<LogicalView*,FieldMask>::aligned*>(
                  &(state->valid_views));
            capture_instances(closer, dirty_mask, valid_views);
          }
        }
      }
      if (!state->reduction_views.empty())
      {
        reduction_mask = state->reduction_mask & capture_mask;
        if (!!reduction_mask)
        {
          // More C++ suckiness
          LegionMap<ReductionView*,FieldMask>::aligned *reduction_views =
            reinterpret_cast<LegionMap<ReductionView*,FieldMask>::aligned*>(
                &(state->reduction_views));
          capture_reductions(reduction_mask, reduction_views);
        }
      }
    }

    //--------------------------------------------------------------------------
    bool CompositeNode::capture_instances(CompositeCloser &closer,
                                          const FieldMask &capture_mask,
                        const LegionMap<LogicalView*,FieldMask>::aligned *views)
    //--------------------------------------------------------------------------
    {
      bool changed = false;
      LegionMap<DeferredView*,FieldMask>::aligned deferred_views;
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            views->begin(); it != views->end(); it++)
      {
        FieldMask overlap = it->second & capture_mask;
        if (!overlap)
          continue;
        // Figure out what kind of view we have
        if (it->first->is_deferred_view())
        {
          deferred_views[it->first->as_deferred_view()] = overlap; 
        }
        else
        { 
          LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
            valid_views.find(it->first);
          if (finder == valid_views.end())
          {
            it->first->add_base_resource_ref(COMPOSITE_NODE_REF);
            valid_views[it->first] = overlap;
          }
          else
            finder->second |= overlap;
        }
      }
      if (!deferred_views.empty())
      {
        // Get a mask for all the fields that we did capture
        FieldMask captured;
        for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
              valid_views.begin(); it != valid_views.end(); it++)
        {
          captured |= it->second;
        }
        // If we captured a real instance for all the fields then we are good
        if (!(capture_mask - captured))
          return changed;
        // Otherwise capture deferred instances for the rest
        for (LegionMap<DeferredView*,FieldMask>::aligned::iterator it = 
              deferred_views.begin(); it != deferred_views.end(); it++)
        {
          if (!!captured)
          {
            it->second -= captured;
            if (!it->second)
              continue;
          }
          // Simplify the composite instance
          DeferredView *simple_view = it->first->simplify(closer, it->second);
          if (simple_view != it->first)
            changed = true;
          LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
            valid_views.find(simple_view);
          if (finder == valid_views.end())
          {
            simple_view->add_base_resource_ref(COMPOSITE_NODE_REF);
            valid_views[simple_view] = it->second; 
          }
          else
            finder->second |= it->second;
        }
      }
      return changed;
    }

    //--------------------------------------------------------------------------
    void CompositeNode::capture_reductions(const FieldMask &capture_mask,
                      const LegionMap<ReductionView*,FieldMask>::aligned *views)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            views->begin(); it != views->end(); it++)
      {
        FieldMask overlap = it->second & capture_mask;
        if (!overlap)
          continue;
        LegionMap<ReductionView*,FieldMask>::aligned::iterator finder = 
          reduction_views.find(it->first);
        if (finder == reduction_views.end())
        {
          it->first->add_base_resource_ref(COMPOSITE_NODE_REF);
          reduction_views[it->first] = overlap;
        }
        else
          finder->second |= overlap;
      }
    }

    //--------------------------------------------------------------------------
    bool CompositeNode::simplify(CompositeCloser &closer,
                                 FieldMask &capture_mask,
                                 CompositeNode *new_parent)
    //--------------------------------------------------------------------------
    {
      // Filter the capture mask
      bool changed = closer.filter_capture_mask(logical_node, capture_mask);
      // If the set of captured nodes changed then we changed
      if (!capture_mask)
        return true;
      CompositeNode *new_node = legion_new<CompositeNode>(logical_node, 
                                                          new_parent);
      new_parent->update_child(new_node, capture_mask);
      // Simplify any of our children
      for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        FieldMask child_capture = it->second & capture_mask;
        if (!child_capture)
        {
          // If the set of nodes captured changes, then we changed
          if (!changed)
            changed = true;
          continue;
        }
        if (it->first->simplify(closer, child_capture, new_node)) 
          changed = true;
      }
      // Now do our capture and update the closer
      if (new_node->capture_instances(closer, capture_mask, &valid_views))
        changed = true;
      new_node->capture_reductions(capture_mask, &reduction_views);
      closer.update_capture_mask(logical_node, capture_mask);
      return changed;
    }

    //--------------------------------------------------------------------------
    void CompositeNode::issue_deferred_copies(const TraversalInfo &info,
                                              MaterializedView *dst,
                                              const FieldMask &copy_mask,
                                            const VersionInfo &src_version_info,
                      const LegionMap<Event,FieldMask>::aligned &preconditions,
                            LegionMap<Event,FieldMask>::aligned &postconditions,
                            LegionMap<Event,FieldMask>::aligned &postreductions,
                                              CopyTracker *tracker, 
                                              CopyAcrossHelper *across_helper,
                                              bool check_root) const
    //--------------------------------------------------------------------------
    {
      // The invariant that we want to maintain for this function is that
      // it places no more than one event in the postconditions data structure
      // for any field.
      LegionMap<Event,FieldMask>::aligned local_postconditions;
      // First see if we are at the root of the tree for this particular copy
      bool traverse_children = true;
      if (check_root)
      {
        CompositeNode *child = find_next_root(dst->logical_node);
        if (child != NULL)
        {
          // If we have another child, we can continue the traversal
          // If we have reductions here we need to do something special
          if (!reduction_views.empty())
          {
            // Have this path fall through to catch the reductions   
            // but don't traverse the children since we're already doing it
            child->issue_deferred_copies(info, dst, copy_mask, src_version_info,
                                  preconditions, local_postconditions, 
                                  postreductions, tracker, across_helper,
                                  true/*check root*/);
            traverse_children = false;
          }
          else // This is the common case
          {
            child->issue_deferred_copies(info, dst, copy_mask, src_version_info,
                                  preconditions, postconditions, postreductions,
                                  tracker, across_helper, true/*check root*/);
            return;
          }
        }
        else
        {
          // Otherwise we fall through and do the actual update copies
          LegionMap<LogicalView*,FieldMask>::aligned all_valid_views;
          // We have to pull down any valid views to make sure we are issuing
          // copies to all the possibly overlapping locations
          find_valid_views(copy_mask, all_valid_views);
          if (!all_valid_views.empty())
          {
            // If we have no children we can just put the results
            // straight into the postcondition otherwise put it
            // in our local postcondition
            if (children.empty() && reduction_views.empty())
            {
              issue_update_copies(info, dst, copy_mask, src_version_info,
                          preconditions, postconditions, all_valid_views, 
                          tracker, across_helper);
              return;
            }
            else
              issue_update_copies(info, dst, copy_mask, src_version_info,
                    preconditions, local_postconditions, all_valid_views, 
                    tracker, across_helper);
          }
        }
      }
      else
      {
        // Issue update copies just from this level that are needed 
        if (!valid_views.empty())
        {
          FieldMask update_mask = dirty_mask & copy_mask;
          if (!!update_mask)
          {
            // If we have no children we can just put the results
            // straight into the postcondition otherwise put it
            // in our local postcondition
            if (children.empty() && reduction_views.empty())
            {
              issue_update_copies(info, dst, update_mask, src_version_info,
                                preconditions, postconditions, valid_views, 
                                tracker, across_helper);
              return;
            }
            else
              issue_update_copies(info, dst, update_mask, src_version_info,
                          preconditions, local_postconditions, valid_views, 
                          tracker, across_helper);
          }
        }
      }
      LegionMap<Event,FieldMask>::aligned temp_preconditions;
      const LegionMap<Event,FieldMask>::aligned *local_preconditions = NULL;
      if (traverse_children)
      {
        // Defer initialization until we find the first interfering child
        for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it =
              children.begin(); it != children.end(); it++)
        {
          FieldMask overlap = it->second & copy_mask;
          if (!overlap)
            continue;
          if (!it->first->logical_node->intersects_with(dst->logical_node))
            continue;
          if (local_preconditions == NULL)
          {
            // Do the initialization
            // The preconditions going down are anything from above
            // as well as anything that we generated
            if (!local_postconditions.empty())
            {
              temp_preconditions = local_postconditions;
              temp_preconditions.insert(preconditions.begin(),
                                        preconditions.end());
              local_preconditions = &temp_preconditions; 
            }
            else
              local_preconditions = &preconditions;
          }
          // Now traverse the child
          it->first->issue_deferred_copies(info, dst, overlap, src_version_info,
                            *local_preconditions, local_postconditions, 
                            postreductions, tracker, across_helper, 
                            false/*check root*/);
        }
      }
      // Handle any reductions we might have
      if (!reduction_views.empty())
      {
        if (local_preconditions != NULL)
          issue_update_reductions(info, dst, copy_mask, src_version_info,
              *local_preconditions, postreductions, tracker, across_helper);
        else if (!local_postconditions.empty())
        {
          temp_preconditions = local_postconditions;
          temp_preconditions.insert(preconditions.begin(),
                                    preconditions.end());
          issue_update_reductions(info, dst, copy_mask, src_version_info,
              temp_preconditions, postreductions, tracker, across_helper);
        }
        else
          issue_update_reductions(info, dst, copy_mask, src_version_info,
              preconditions, postreductions, tracker, across_helper);
      }
      // Quick out if we don't have any postconditions
      if (local_postconditions.empty())
        return;
      // See if we actually traversed any children
      if (local_preconditions != NULL)
      {
        // We traversed some children so we need to do a merge of our
        // local_postconditions to deduplicate events across fields
        LegionList<EventSet>::aligned event_sets; 
        RegionTreeNode::compute_event_sets(copy_mask, local_postconditions,
                                           event_sets);
        for (LegionList<EventSet>::aligned::const_iterator it = 
              event_sets.begin(); it != event_sets.end(); it++)
        {
          if (it->preconditions.size() == 1)
          {
            Event post = *(it->preconditions.begin());
            if (!post.exists())
              continue;
            postconditions[post] = it->set_mask;
          }
          else
          {
            Event post = Runtime::merge_events<false>(it->preconditions);
            if (!post.exists())
              continue;
            postconditions[post] = it->set_mask;
          }
        }
      }
      else
      {
        // We didn't traverse any children so we can just copy our
        // local_postconditions into the postconditions set
        postconditions.insert(local_postconditions.begin(),
                              local_postconditions.end());
      }
    }

    //--------------------------------------------------------------------------
    CompositeNode* CompositeNode::find_next_root(RegionTreeNode *target) const
    //--------------------------------------------------------------------------
    {
      if (children.empty())
        return NULL;
      if (children.size() == 1)
      {
        CompositeNode *child = children.begin()->first;
        if (child->logical_node->dominates(target))
          return child;
      }
      else if (logical_node->are_all_children_disjoint())
      {
        for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it =
              children.begin(); it != children.end(); it++)
        {
          if (it->first->logical_node->dominates(target))
            return it->first;
        }
      }
      else
      {
        CompositeNode *child = NULL;
        // Check to see if we have one child that dominates and none
        // that intersect
        for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it =
              children.begin(); it != children.end(); it++)
        {
          if (it->first->logical_node->dominates(target))
          {
            // Having multiple dominating children is not allowed
            if (child != NULL)
              return NULL;
            child = it->first;
            continue;
          }
          // If it doesn't dominate, but it does intersect that is not allowed
          if (it->first->logical_node->intersects_with(target))
            return NULL;
        }
        return child;
      }
      return NULL;
    }

    //--------------------------------------------------------------------------
    void CompositeNode::find_valid_views(const FieldMask &search_mask,
                        LegionMap<LogicalView*,FieldMask>::aligned &valid) const
    //--------------------------------------------------------------------------
    {
      bool need_check = false;
      if (parent != NULL)
      {
        FieldMask up_mask = search_mask - dirty_mask;
        if (!!up_mask)
        {
          LegionMap<LogicalView*,FieldMask>::aligned valid_up;
          parent->find_valid_views(up_mask, valid_up);
          if (!valid_up.empty())
          {
            need_check = true;
            const ColorPoint &local_color = logical_node->get_color();
            for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it =
                  valid_up.begin(); it != valid_up.end(); it++)
            {
              LogicalView *local_view = it->first->get_subview(local_color);
              valid[local_view] = it->second;
            }
          }
        }
      }
      // Now figure out which of our views we can add
      if (!valid_views.empty())
      {
        for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
              valid_views.begin(); it != valid_views.end(); it++)
        {
          FieldMask overlap = search_mask & it->second;
          if (!overlap)
            continue;
          if (need_check)
          {
            LegionMap<LogicalView*,FieldMask>::aligned::iterator finder = 
              valid.find(it->first);
            if (finder == valid.end())
              valid[it->first] = overlap;
            else
              finder->second |= overlap;
          }
          else
            valid[it->first] = overlap;
        }
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::issue_update_copies(const TraversalInfo &info, 
                                            MaterializedView *dst,
                                            FieldMask copy_mask,
                                            const VersionInfo &src_version_info,
                  const LegionMap<Event,FieldMask>::aligned &preconditions,
                        LegionMap<Event,FieldMask>::aligned &postconditions,
                  const LegionMap<LogicalView*,FieldMask>::aligned &views,
                    CopyTracker *tracker, CopyAcrossHelper *across_helper) const
    //--------------------------------------------------------------------------
    {
      // This is similar to the version of this call in RegionTreeNode
      // but different in that it knows how to deal with intersections
      // Do a quick check to see if we are done early
      {
        LegionMap<LogicalView*,FieldMask>::aligned::const_iterator finder = 
          views.find(dst);
        if (finder != views.end())
        {
          copy_mask -= finder->second;
          if (!copy_mask)
            return;
        }
      }
      LegionMap<MaterializedView*,FieldMask>::aligned src_instances;
      LegionMap<DeferredView*,FieldMask>::aligned deferred_instances;
      // Sort the instances
      dst->logical_node->sort_copy_instances(info, dst, copy_mask, views,
                                             src_instances, deferred_instances);
      // Now we can issue the copy operations
      if (!src_instances.empty())
      {
        // This has all our destination preconditions
        // Only issue copies from fields which have values
        FieldMask actual_copy_mask;
        LegionMap<Event,FieldMask>::aligned src_preconditions;
        for (LegionMap<MaterializedView*,FieldMask>::aligned::const_iterator 
              it = src_instances.begin(); it != src_instances.end(); it++)
        {
          it->first->find_copy_preconditions(0/*redop*/, true/*reading*/,
                                             it->second, src_version_info,
                                             src_preconditions);
          actual_copy_mask |= it->second;
        }
        FieldMask diff_mask = copy_mask - actual_copy_mask;
        if (!!diff_mask)
        {
          // Move in any preconditions that overlap with our set of fields
          for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
                preconditions.begin(); it != preconditions.end(); it++)
          {
            FieldMask overlap = it->second & actual_copy_mask;
            if (!overlap)
              continue;
            // If we ever hit this assertion we need to merge
#ifdef DEBUG_HIGH_LEVEL
            assert(src_preconditions.find(it->first) ==
                   src_preconditions.end());
#endif
            src_preconditions[it->first] = overlap;
          }
        }
        else // we can just add all the preconditions
        {
          for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
                preconditions.begin(); it != preconditions.end(); it++)
          {
            // If we ever hit this assertion we need to merge
#ifdef DEBUG_HIGH_LEVEL
            assert(src_preconditions.find(it->first) ==
                   src_preconditions.end());
#endif
            src_preconditions[it->first] = it->second;
          }
        }
        // issue the grouped copies and put the result in the postconditions
        // We are the intersect
        dst->logical_node->issue_grouped_copies(info, dst, src_preconditions,
                                 actual_copy_mask, src_instances, 
                                 src_version_info, postconditions, tracker, 
                                 across_helper, logical_node);
      }
      if (!deferred_instances.empty())
      {
        // If we have any deferred instances, issue copies to them as well
        for (LegionMap<DeferredView*,FieldMask>::aligned::const_iterator it = 
              deferred_instances.begin(); it != deferred_instances.end(); it++)
        {
          it->first->issue_deferred_copies(info, dst, it->second,
                        preconditions, postconditions, tracker, across_helper);
        }
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::issue_update_reductions(const TraversalInfo &info,
                                                MaterializedView *dst,
                                                const FieldMask &copy_mask,
                                            const VersionInfo &src_version_info,
                      const LegionMap<Event,FieldMask>::aligned &preconditions,
                            LegionMap<Event,FieldMask>::aligned &postreductions,
                    CopyTracker *tracker, CopyAcrossHelper *across_helper) const
    //--------------------------------------------------------------------------
    {
      FieldMask reduce_mask = copy_mask & reduction_mask;
      if (!reduce_mask)
        return;
      std::set<Event> local_preconditions;
      for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
            preconditions.begin(); it != preconditions.end(); it++)
      {
        if (it->second * reduce_mask)
          continue;
        local_preconditions.insert(it->first);
      }
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            reduction_views.begin(); it != reduction_views.end(); it++)
      {
        FieldMask overlap = reduce_mask & it->second;
        if (!overlap)
          continue;
        // Perform the reduction
        Event reduce_event = it->first->perform_deferred_reduction(dst,
            reduce_mask, src_version_info, local_preconditions, info.op,
            across_helper, dst->logical_node == it->first->logical_node ?
            NULL : it->first->logical_node);
        if (reduce_event.exists())
        {
          postreductions[reduce_event] = overlap;
          if (tracker != NULL)
            tracker->add_copy_event(reduce_event);
        }
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::pack_composite_tree(Serializer &rez, 
                                            AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      rez.serialize(dirty_mask);
      rez.serialize(reduction_mask);
      rez.serialize<size_t>(valid_views.size());
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            valid_views.begin(); it != valid_views.end(); it++)
      {
        rez.serialize(it->first->did);
        rez.serialize(it->second);
      }
      rez.serialize<size_t>(reduction_views.size());
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            reduction_views.begin(); it != reduction_views.end(); it++)
      {
        // Same as above 
        rez.serialize(it->first->did);
        rez.serialize(it->second);
      }
      rez.serialize<size_t>(children.size());
      for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        rez.serialize(it->first->logical_node->get_color());
        rez.serialize(it->second);
        it->first->pack_composite_tree(rez, target);
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::unpack_composite_tree(Deserializer &derez,
                                              AddressSpaceID source,
                                              Runtime *runtime,
                                              std::set<Event> &ready_events,
                                  std::map<LogicalView*,unsigned> &pending_refs)
    //--------------------------------------------------------------------------
    {
      derez.deserialize(dirty_mask);
      derez.deserialize(reduction_mask);
      size_t num_views;
      derez.deserialize(num_views);
      for (unsigned idx = 0; idx < num_views; idx++)
      {
        DistributedID view_did;
        derez.deserialize(view_did);
        Event ready = Event::NO_EVENT;
        LogicalView *view = 
          runtime->find_or_request_logical_view(view_did, ready);
        derez.deserialize(valid_views[view]);
        if (ready.exists())
        {
          ready_events.insert(ready);
          std::map<LogicalView*,unsigned>::iterator finder = 
            pending_refs.find(view);
          if (finder == pending_refs.end())
            pending_refs[view] = 1;
          else
            finder->second++;
          continue;
        }
        view->add_base_resource_ref(COMPOSITE_NODE_REF);
      }
      size_t num_reductions;
      derez.deserialize(num_reductions);
      for (unsigned idx = 0; idx < num_reductions; idx++)
      {
        DistributedID reduc_did;
        derez.deserialize(reduc_did);
        Event ready = Event::NO_EVENT;
        LogicalView *view = 
          runtime->find_or_request_logical_view(reduc_did, ready);
        // Have to static cast since it might not be ready yet
        ReductionView *red_view = static_cast<ReductionView*>(view);
        derez.deserialize(reduction_views[red_view]);
        if (ready.exists())
        {
          ready_events.insert(ready);
          std::map<LogicalView*,unsigned>::iterator finder = 
            pending_refs.find(view);
          if (finder == pending_refs.end())
            pending_refs[view] = 1;
          else
            finder->second++;
          continue;
        }
        red_view->add_base_resource_ref(COMPOSITE_NODE_REF);
      }
      size_t num_children;
      derez.deserialize(num_children);
      for (unsigned idx = 0; idx < num_children; idx++)
      {
        ColorPoint child_color;
        derez.deserialize(child_color);
        RegionTreeNode *child_node = logical_node->get_tree_child(child_color);
        CompositeNode *child = legion_new<CompositeNode>(child_node, this);
        derez.deserialize(children[child]);
        child->unpack_composite_tree(derez, source, runtime, 
                                     ready_events, pending_refs);
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::make_local(std::set<Event> &preconditions,
                                   std::set<DistributedID> &checked_views)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            valid_views.begin(); it != valid_views.end(); it++)
      {
        if (it->first->is_deferred_view())
        {
          DeferredView *def_view = it->first->as_deferred_view();
          if (def_view->is_composite_view() &&
              (checked_views.find(it->first->did) == checked_views.end()))
          {
            def_view->as_composite_view()->make_local(preconditions);
            checked_views.insert(it->first->did);
          }
        }
      }
      // Then traverse any children
      for (LegionMap<CompositeNode*,FieldMask>::aligned::const_iterator it =
            children.begin(); it != children.end(); it++)
      {
        it->first->make_local(preconditions, checked_views);
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::notify_active(void)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it =
            valid_views.begin(); it != valid_views.end(); it++)
      {
        it->first->add_nested_gc_ref(owner_did);
      }
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            reduction_views.begin(); it != reduction_views.end(); it++)
      {
        it->first->add_nested_gc_ref(owner_did);
      }
      for (std::map<CompositeNode*,FieldMask>::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        it->first->notify_active();
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::notify_inactive(void)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            valid_views.end(); it != valid_views.end(); it++)
      {
        // Don't worry about deletion condition since we own resource refs
        it->first->remove_nested_gc_ref(owner_did);
      }
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            reduction_views.begin(); it != reduction_views.end(); it++)
      {
        // Don't worry about deletion condition since we own resource refs
        it->first->remove_nested_gc_ref(owner_did);
      }
      for (std::map<CompositeNode*,FieldMask>::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        it->first->notify_inactive();
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::notify_valid(void)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it =
            valid_views.begin(); it != valid_views.end(); it++)
      {
        it->first->add_nested_valid_ref(owner_did);
      }
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it = 
            reduction_views.begin(); it != reduction_views.end(); it++)
      {
        it->first->add_nested_valid_ref(owner_did);
      }
      for (std::map<CompositeNode*,FieldMask>::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        it->first->notify_valid();
      }
    }

    //--------------------------------------------------------------------------
    void CompositeNode::notify_invalid(void)
    //--------------------------------------------------------------------------
    {
      for (LegionMap<LogicalView*,FieldMask>::aligned::const_iterator it = 
            valid_views.end(); it != valid_views.end(); it++)
      {
        // Don't worry about deletion condition since we own resource refs
        it->first->add_nested_valid_ref(owner_did);
      }
      for (LegionMap<ReductionView*,FieldMask>::aligned::const_iterator it =
            reduction_views.begin(); it != reduction_views.end(); it++)
      {
        // Don't worry about deletion condition since we own resource refs
        it->first->add_nested_valid_ref(owner_did);
      }
      for (std::map<CompositeNode*,FieldMask>::const_iterator it = 
            children.begin(); it != children.end(); it++)
      {
        it->first->notify_invalid();
      }
    }

    /////////////////////////////////////////////////////////////
    // FillView 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    FillView::FillView(RegionTreeForest *ctx, DistributedID did,
                       AddressSpaceID owner_proc, AddressSpaceID local_proc,
                       RegionTreeNode *node, FillViewValue *val)
      : DeferredView(ctx, encode_fill_did(did), 
                     owner_proc, local_proc, node), value(val)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(value != NULL);
#endif
      value->add_reference();
      if (!is_owner())
      {
        add_base_resource_ref(REMOTE_DID_REF);
        send_remote_registration();
      }
#ifdef LEGION_GC
      log_garbage.info("GC Fill View %ld", did);
#endif
    }

    //--------------------------------------------------------------------------
    FillView::FillView(const FillView &rhs)
      : DeferredView(NULL, 0, 0, 0, NULL), value(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }
    
    //--------------------------------------------------------------------------
    FillView::~FillView(void)
    //--------------------------------------------------------------------------
    {
      if (value->remove_reference())
        delete value;
      if (is_owner())
      {
        UpdateReferenceFunctor<RESOURCE_REF_KIND,false/*add*/> functor(this);
        map_over_remote_instances(functor);
      }
    }

    //--------------------------------------------------------------------------
    FillView& FillView::operator=(const FillView &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LogicalView* FillView::get_subview(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // Fill views don't need subviews
      return this;
    }

    //--------------------------------------------------------------------------
    void FillView::notify_active(void)
    //--------------------------------------------------------------------------
    {
      // Nothing to do
    }

    //--------------------------------------------------------------------------
    void FillView::notify_inactive(void)
    //--------------------------------------------------------------------------
    {
      // Nothing to do
    }
    
    //--------------------------------------------------------------------------
    void FillView::notify_valid(void)
    //--------------------------------------------------------------------------
    {
      // Nothing to do
    }

    //--------------------------------------------------------------------------
    void FillView::notify_invalid(void)
    //--------------------------------------------------------------------------
    {
      // Nothing to do
    }

    //--------------------------------------------------------------------------
    void FillView::send_view(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(is_owner());
      assert(logical_node->is_region());
#endif
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(did);
        rez.serialize(owner_space);
        rez.serialize(logical_node->as_region_node()->handle);
        rez.serialize(value->value_size);
        rez.serialize(value->value, value->value_size);
      }
      runtime->send_fill_view(target, rez);
      // We've now done the send so record it
      update_remote_instances(target);
    }

    //--------------------------------------------------------------------------
    void FillView::send_view_updates(AddressSpaceID target, 
                                     const FieldMask &update_mask)
    //--------------------------------------------------------------------------
    {
      // Nothing to do since we don't have any views that get updated
    }

    //--------------------------------------------------------------------------
    DeferredView* FillView::simplify(CompositeCloser &closer, 
                                     const FieldMask &capture_mask)
    //--------------------------------------------------------------------------
    {
      // Fill views simplify easily
      return this;
    }

    //--------------------------------------------------------------------------
    void FillView::issue_deferred_copies(const TraversalInfo &info,
                                         MaterializedView *dst,
                                         const FieldMask &copy_mask,
                      const LegionMap<Event,FieldMask>::aligned &preconditions,
                            LegionMap<Event,FieldMask>::aligned &postconditions,
                                         CopyTracker *tracker,
                                         CopyAcrossHelper *across_helper)
    //--------------------------------------------------------------------------
    {
      // Compute the precondition sets
      LegionList<EventSet>::aligned precondition_sets;
      RegionTreeNode::compute_event_sets(copy_mask, preconditions,
                                         precondition_sets);
      // Iterate over the precondition sets
      for (LegionList<EventSet>::aligned::iterator pit = 
            precondition_sets.begin(); pit !=
            precondition_sets.end(); pit++)
      {
        EventSet &pre_set = *pit;
        // Build the src and dst fields vectors
        std::vector<Domain::CopySrcDstField> dst_fields;
        dst->copy_to(pre_set.set_mask, dst_fields, across_helper);
        Event fill_pre = Runtime::merge_events<false>(pre_set.preconditions);
        // Issue the fill command
        // Only apply an intersection if the destination logical node
        // is different than our logical node
        Event fill_post = dst->logical_node->issue_fill(info.op, dst_fields,
                                  value->value, value->value_size, fill_pre, 
                  (logical_node == dst->logical_node) ? NULL : logical_node);
        if (fill_post.exists())
        {
          if (tracker != NULL)
            tracker->add_copy_event(fill_post);
          postconditions[fill_post] = pre_set.set_mask;
        }
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ void FillView::handle_send_fill_view(Runtime *runtime,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      AddressSpaceID owner_space;
      derez.deserialize(owner_space);
      LogicalRegion handle;
      derez.deserialize(handle);
      size_t value_size;
      derez.deserialize(value_size);
      void *value = malloc(value_size);
      derez.deserialize(value, value_size);
      
      RegionNode *target_node = runtime->forest->get_node(handle);
      FillView::FillViewValue *fill_value = 
                      new FillView::FillViewValue(value, value_size);
      void *location;
      if (runtime->find_pending_collectable_location(did, location))
        legion_new_in_place<FillView>(location, runtime->forest, did,
                                      owner_space, runtime->address_space,
                                      target_node, fill_value);
      else
        legion_new<FillView>(runtime->forest, did, owner_space,
                             runtime->address_space, target_node, fill_value);
    }

    /////////////////////////////////////////////////////////////
    // ReductionView 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    ReductionView::ReductionView(RegionTreeForest *ctx, DistributedID did,
                                 AddressSpaceID own_sp, AddressSpaceID loc_sp,
                                 RegionTreeNode *node, ReductionManager *man,
                                 SingleTask *own_ctx)
      : InstanceView(ctx, encode_reduction_did(did), 
                     own_sp, loc_sp, node, own_ctx),
        manager(man)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(manager != NULL);
#endif
      logical_node->register_instance_view(manager, owner_context, this);
      manager->add_nested_resource_ref(did);
      if (!is_owner())
      {
        add_base_resource_ref(REMOTE_DID_REF);
        send_remote_registration();
      }
#ifdef LEGION_GC
      log_garbage.info("GC Reduction View %ld %ld", did, manager->did);
#endif
    }

    //--------------------------------------------------------------------------
    ReductionView::ReductionView(const ReductionView &rhs)
      : InstanceView(NULL, 0, 0, 0, NULL, NULL), manager(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    ReductionView::~ReductionView(void)
    //--------------------------------------------------------------------------
    {
      // Always unregister ourselves with the region tree node
      logical_node->unregister_instance_view(manager, owner_context);
      if (is_owner())
      {
        // If we're the owner, remove our valid references on remote nodes
        UpdateReferenceFunctor<RESOURCE_REF_KIND,false/*add*/> functor(this);
        map_over_remote_instances(functor);
      }
      if (manager->remove_nested_resource_ref(did))
      {
        if (manager->is_list_manager())
          legion_delete(manager->as_list_manager());
        else
          legion_delete(manager->as_fold_manager());
      }
      // Remove any initial users as well
      if (!initial_user_events.empty())
      {
        for (std::set<Event>::const_iterator it = initial_user_events.begin();
              it != initial_user_events.end(); it++)
          filter_local_users(*it);
      }
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE) && \
      defined(DEBUG_HIGH_LEVEL)
      assert(reduction_users.empty());
      assert(reading_users.empty());
      assert(outstanding_gc_events.empty());
#endif
    }

    //--------------------------------------------------------------------------
    ReductionView& ReductionView::operator=(const ReductionView &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void ReductionView::perform_reduction(InstanceView *target,
                                          const FieldMask &reduce_mask,
                                          const VersionInfo &version_info,
                                          Operation *op,
                                          CopyTracker *tracker /*= NULL*/)
    //--------------------------------------------------------------------------
    {
      std::vector<Domain::CopySrcDstField> src_fields;
      std::vector<Domain::CopySrcDstField> dst_fields;
      bool fold = target->reduce_to(manager->redop, reduce_mask, dst_fields);
      this->reduce_from(manager->redop, reduce_mask, src_fields);

      LegionMap<Event,FieldMask>::aligned preconditions;
      target->find_copy_preconditions(manager->redop, false/*reading*/, 
                                      reduce_mask, version_info, preconditions);
      this->find_copy_preconditions(manager->redop, true/*reading*/, 
                                    reduce_mask, version_info, preconditions);
      std::set<Event> event_preconds;
      for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
            preconditions.begin(); it != preconditions.end(); it++)
      {
        event_preconds.insert(it->first);
      }
      Event reduce_pre = Runtime::merge_events<false>(event_preconds); 
      Event reduce_post = manager->issue_reduction(op, src_fields, dst_fields,
                                                   target->logical_node, 
                                                   reduce_pre,
                                                   fold, true/*precise*/,
                                                   NULL/*intersect*/);
      target->add_copy_user(manager->redop, reduce_post, version_info,
                            reduce_mask, false/*reading*/);
      this->add_copy_user(manager->redop, reduce_post, version_info,
                          reduce_mask, true/*reading*/);
      if (tracker != NULL)
        tracker->add_copy_event(reduce_post);
    } 

    //--------------------------------------------------------------------------
    Event ReductionView::perform_deferred_reduction(MaterializedView *target,
                                                    const FieldMask &red_mask,
                                                const VersionInfo &version_info,
                                                    const std::set<Event> &pre,
                                                    Operation *op,
                                                    CopyAcrossHelper *helper,
                                                    RegionTreeNode *intersect)
    //--------------------------------------------------------------------------
    {
      std::vector<Domain::CopySrcDstField> src_fields;
      std::vector<Domain::CopySrcDstField> dst_fields;
      bool fold = target->reduce_to(manager->redop, red_mask, 
                                    dst_fields, helper);
      this->reduce_from(manager->redop, red_mask, src_fields);

      LegionMap<Event,FieldMask>::aligned src_pre;
      // Don't need to ask the target for preconditions as they 
      // are included as part of the pre set
      find_copy_preconditions(manager->redop, true/*reading*/,
                              red_mask, version_info, src_pre);
      std::set<Event> preconditions = pre;
      for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
            src_pre.begin(); it != src_pre.end(); it++)
      {
        preconditions.insert(it->first);
      }
      Event reduce_pre = Runtime::merge_events<false>(preconditions); 
      Event reduce_post = target->logical_node->issue_copy(op,
                                            src_fields, dst_fields,
                                            reduce_pre, intersect,
                                            manager->redop, fold);
      // No need to add the user to the destination as that will
      // be handled by the caller using the reduce post event we return
      add_copy_user(manager->redop, reduce_post, version_info,
                    red_mask, true/*reading*/);
      return reduce_post;
    }

    //--------------------------------------------------------------------------
    Event ReductionView::perform_deferred_across_reduction(
                              MaterializedView *target, FieldID dst_field, 
                              FieldID src_field, unsigned src_index, 
                              const VersionInfo &version_info,
                              const std::set<Event> &preconds,
                              Operation *op, RegionTreeNode *intersect)
    //--------------------------------------------------------------------------
    {
      std::vector<Domain::CopySrcDstField> src_fields;
      std::vector<Domain::CopySrcDstField> dst_fields;
      const bool fold = false;
      target->copy_field(dst_field, dst_fields);
      FieldMask red_mask; red_mask.set_bit(src_index);
      this->reduce_from(manager->redop, red_mask, src_fields);

      LegionMap<Event,FieldMask>::aligned src_pre;
      // Don't need to ask the target for preconditions as they 
      // are included as part of the pre set
      find_copy_preconditions(manager->redop, true/*reading*/,
                              red_mask, version_info, src_pre);
      std::set<Event> preconditions = preconds;
      for (LegionMap<Event,FieldMask>::aligned::const_iterator it = 
            src_pre.begin(); it != src_pre.end(); it++)
      {
        preconditions.insert(it->first);
      }
      Event reduce_pre = Runtime::merge_events<false>(preconditions); 
      Event reduce_post = manager->issue_reduction(op, src_fields, dst_fields,
                                                   intersect, reduce_pre,
                                                   fold, false/*precise*/,
                                                   target->logical_node);
      // No need to add the user to the destination as that will
      // be handled by the caller using the reduce post event we return
      add_copy_user(manager->redop, reduce_post, version_info,
                    red_mask, true/*reading*/);
      return reduce_post;
    }

    //--------------------------------------------------------------------------
    PhysicalManager* ReductionView::get_manager(void) const
    //--------------------------------------------------------------------------
    {
      return manager;
    }

    //--------------------------------------------------------------------------
    LogicalView* ReductionView::get_subview(const ColorPoint &c)
    //--------------------------------------------------------------------------
    {
      // Right now we don't make sub-views for reductions
      return this;
    }

    //--------------------------------------------------------------------------
    void ReductionView::find_copy_preconditions(ReductionOpID redop,
                                                bool reading,
                                                const FieldMask &copy_mask,
                                                const VersionInfo &version_info,
                             LegionMap<Event,FieldMask>::aligned &preconditions)
    //--------------------------------------------------------------------------
    {
      Event use_event = manager->get_use_event();
      if (use_event.exists())
      {
        LegionMap<Event,FieldMask>::aligned::iterator finder = 
            preconditions.find(use_event);
        if (finder == preconditions.end())
          preconditions[use_event] = copy_mask;
        else
          finder->second |= copy_mask;
      }
      AutoLock v_lock(view_lock,1,false/*exclusive*/);
      if (reading)
      {
        // Register dependences on any reducers
        for (LegionMap<Event,EventUsers>::aligned::const_iterator rit = 
              reduction_users.begin(); rit != reduction_users.end(); rit++)
        {
          const EventUsers &event_users = rit->second;
          if (event_users.single)
          {
            FieldMask overlap = copy_mask & event_users.user_mask;
            if (!overlap)
              continue;
            LegionMap<Event,FieldMask>::aligned::iterator finder = 
              preconditions.find(rit->first);
            if (finder == preconditions.end())
              preconditions[rit->first] = overlap;
            else
              finder->second |= overlap;
          }
          else
          {
            if (!(copy_mask * event_users.user_mask))
            {
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                    it = event_users.users.multi_users->begin(); it !=
                    event_users.users.multi_users->end(); it++)
              {
                FieldMask overlap = copy_mask & it->second;
                if (!overlap)
                  continue;
                LegionMap<Event,FieldMask>::aligned::iterator finder = 
                  preconditions.find(rit->first);
                if (finder == preconditions.end())
                  preconditions[rit->first] = overlap;
                else
                  finder->second |= overlap;
              }
            }
          }
        }
      }
      else
      {
        // Register dependences on any readers
        for (LegionMap<Event,EventUsers>::aligned::const_iterator rit = 
              reading_users.begin(); rit != reading_users.end(); rit++)
        {
          const EventUsers &event_users = rit->second;
          if (event_users.single)
          {
            FieldMask overlap = copy_mask & event_users.user_mask;
            if (!overlap)
              continue;
            LegionMap<Event,FieldMask>::aligned::iterator finder = 
              preconditions.find(rit->first);
            if (finder == preconditions.end())
              preconditions[rit->first] = overlap;
            else
              finder->second |= overlap;
          }
          else
          {
            if (!(copy_mask * event_users.user_mask))
            {
              for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                    it = event_users.users.multi_users->begin(); it !=
                    event_users.users.multi_users->end(); it++)
              {
                FieldMask overlap = copy_mask & it->second;
                if (!overlap)
                  continue;
                LegionMap<Event,FieldMask>::aligned::iterator finder = 
                  preconditions.find(rit->first);
                if (finder == preconditions.end())
                  preconditions[rit->first] = overlap;
                else
                  finder->second |= overlap;
              }
            }
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void ReductionView::add_copy_user(ReductionOpID redop, Event copy_term,
                                      const VersionInfo &version_info,
                                      const FieldMask &mask, bool reading)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(redop == manager->redop);
#endif
      
      // Quick test: only need to do this if copy term exists
      bool issue_collect = false;
      if (copy_term.exists())
      {
        PhysicalUser *user;
        // We don't use field versions for doing interference 
        // tests on reductions so no need to record it
        if (reading)
        {
          RegionUsage usage(READ_ONLY, EXCLUSIVE, 0);
          user = legion_new<PhysicalUser>(usage, ColorPoint());
        }
        else
        {
          RegionUsage usage(REDUCE, EXCLUSIVE, redop);
          user = legion_new<PhysicalUser>(usage, ColorPoint());
        }
        AutoLock v_lock(view_lock);
        add_physical_user(user, reading, copy_term, mask);
        // Update the reference users
        if (outstanding_gc_events.find(copy_term) ==
            outstanding_gc_events.end())
        {
          outstanding_gc_events.insert(copy_term);
          issue_collect = true;
        }
      }
      // Launch the garbage collection task if necessary
      if (issue_collect)
        defer_collect_user(copy_term);
    }

    //--------------------------------------------------------------------------
    Event ReductionView::add_user(const RegionUsage &usage, Event term_event,
                                  const FieldMask &user_mask, Operation *op,
                                  const VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      if (IS_REDUCE(usage))
        assert(usage.redop == manager->redop);
      else
        assert(IS_READ_ONLY(usage));
#endif
      const bool reading = IS_READ_ONLY(usage);
      std::set<Event> wait_on;
      Event use_event = manager->get_use_event();
      if (use_event.exists())
        wait_on.insert(use_event);
      // Who cares just hold the lock in exlcusive mode, this analysis
      // shouldn't be too expensive for reduction views
      bool issue_collect = false;
      PhysicalUser *new_user;
      // We don't use field versions for doing interference 
      // tests on reductions so no need to record it
      if (reading)
        new_user = legion_new<PhysicalUser>(usage, ColorPoint());
      else
        new_user = legion_new<PhysicalUser>(usage, ColorPoint());
      {
        AutoLock v_lock(view_lock);
        if (!reading)
        {
          // Reducing
          for (LegionMap<Event,EventUsers>::aligned::const_iterator rit = 
                reading_users.begin(); rit != reading_users.end(); rit++)
          {
            const EventUsers &event_users = rit->second;
            if (event_users.single)
            {
              FieldMask overlap = user_mask & event_users.user_mask;
              if (!overlap)
                continue;
              wait_on.insert(rit->first);
            }
            else
            {
              if (!(user_mask * event_users.user_mask))
              {
                for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator
                      it = event_users.users.multi_users->begin(); it !=
                      event_users.users.multi_users->end(); it++)
                {
                  FieldMask overlap = user_mask & it->second;
                  if (!overlap)
                    continue;
                  // Once we have one event precondition we are done
                  wait_on.insert(rit->first);
                  break;
                }
              }
            }
          }
          add_physical_user(new_user, false/*reading*/, term_event, user_mask);
        }
        else // We're reading so wait on any reducers
        {
          for (LegionMap<Event,EventUsers>::aligned::const_iterator rit = 
                reduction_users.begin(); rit != reduction_users.end(); rit++)
          {
            const EventUsers &event_users = rit->second;
            if (event_users.single)
            {
              FieldMask overlap = user_mask & event_users.user_mask;
              if (!overlap)
                continue;
              wait_on.insert(rit->first);
            }
            else
            {
              if (!(user_mask * event_users.user_mask))
              {
                for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                      it = event_users.users.multi_users->begin(); it !=
                      event_users.users.multi_users->end(); it++)
                {
                  FieldMask overlap = user_mask & it->second;
                  if (!overlap)
                    continue;
                  // Once we have one event precondition we are done
                  wait_on.insert(rit->first);
                  break;
                }
              }
            }
          }
          add_physical_user(new_user, true/*reading*/, term_event, user_mask);
        }
        // Only need to do this if we actually have a term event
        if (outstanding_gc_events.find(term_event) ==
            outstanding_gc_events.end())
        {
          outstanding_gc_events.insert(term_event);
          issue_collect = true;
        }
      }
      // Launch the garbage collection task if we need to
      if (issue_collect)
        defer_collect_user(term_event);
      // Return our result
      return Runtime::merge_events<false>(wait_on);
    }

    //--------------------------------------------------------------------------
    void ReductionView::add_physical_user(PhysicalUser *user, bool reading,
                                          Event term_event, 
                                          const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      // Better already be holding the lock
      EventUsers *event_users;
      if (reading)
        event_users = &(reading_users[term_event]);
      else
        event_users = &(reduction_users[term_event]);
      if (event_users->single)
      {
        if (event_users->users.single_user == NULL)
        {
          // make it the entry
          event_users->users.single_user = user;
          event_users->user_mask = user_mask;
        }
        else
        {
          // convert to multi
          LegionMap<PhysicalUser*,FieldMask>::aligned *new_map = 
                           new LegionMap<PhysicalUser*,FieldMask>::aligned();
          (*new_map)[event_users->users.single_user] = event_users->user_mask;
          (*new_map)[user] = user_mask;
          event_users->user_mask |= user_mask;
          event_users->users.multi_users = new_map;
          event_users->single = false;
        }
      }
      else
      {
        // Add it to the set 
        (*event_users->users.multi_users)[user] = user_mask;
        event_users->user_mask |= user_mask;
      }
    }

    //--------------------------------------------------------------------------
    void ReductionView::filter_local_users(Event term_event)
    //--------------------------------------------------------------------------
    {
      // Better be holding the lock before calling this
      std::set<Event>::iterator event_finder = 
        outstanding_gc_events.find(term_event);
      if (event_finder != outstanding_gc_events.end())
      {
        LegionMap<Event,EventUsers>::aligned::iterator finder = 
          reduction_users.find(term_event);
        if (finder != reduction_users.end())
        {
          EventUsers &event_users = finder->second;
          if (event_users.single)
          {
            legion_delete(event_users.users.single_user);
          }
          else
          {
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator it
                  = event_users.users.multi_users->begin(); it !=
                  event_users.users.multi_users->end(); it++)
            {
              legion_delete(it->first);
            }
            delete event_users.users.multi_users;
          }
          reduction_users.erase(finder);
        }
        finder = reading_users.find(term_event);
        if (finder != reading_users.end())
        {
          EventUsers &event_users = finder->second;
          if (event_users.single)
          {
            legion_delete(event_users.users.single_user);
          }
          else
          {
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator 
                  it = event_users.users.multi_users->begin(); it !=
                  event_users.users.multi_users->end(); it++)
            {
              legion_delete(it->first);
            }
            delete event_users.users.multi_users;
          }
          reading_users.erase(finder);
        }
        outstanding_gc_events.erase(event_finder);
      }
    }

    //--------------------------------------------------------------------------
    void ReductionView::add_initial_user(Event term_event, 
                                         const RegionUsage &usage,
                                         const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      // We don't use field versions for doing interference tests on
      // reductions so there is no need to record it
      PhysicalUser *user = legion_new<PhysicalUser>(usage, ColorPoint()); 
      add_physical_user(user, IS_READ_ONLY(usage), term_event, user_mask);
      initial_user_events.insert(term_event);
      // Don't need to actual launch a collection task, destructor
      // will handle this case
      outstanding_gc_events.insert(term_event);
    }
 
    //--------------------------------------------------------------------------
    bool ReductionView::reduce_to(ReductionOpID redop, 
                                  const FieldMask &reduce_mask,
                              std::vector<Domain::CopySrcDstField> &dst_fields,
                                  CopyAcrossHelper *across_helper)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(redop == manager->redop);
#endif
      // Get the destination fields for this copy
      if (across_helper == NULL)
        manager->find_field_offsets(reduce_mask, dst_fields);
      else
        across_helper->compute_across_offsets(reduce_mask, dst_fields);
      return manager->is_foldable();
    }

    //--------------------------------------------------------------------------
    void ReductionView::reduce_from(ReductionOpID redop,
                                    const FieldMask &reduce_mask,
                              std::vector<Domain::CopySrcDstField> &src_fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(redop == manager->redop);
#endif
      manager->find_field_offsets(reduce_mask, src_fields);
    }

    //--------------------------------------------------------------------------
    void ReductionView::copy_to(const FieldMask &copy_mask,
                               std::vector<Domain::CopySrcDstField> &dst_fields,
                                CopyAcrossHelper *across_helper)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    void ReductionView::copy_from(const FieldMask &copy_mask,
                               std::vector<Domain::CopySrcDstField> &src_fields)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    bool ReductionView::has_war_dependence(const RegionUsage &usage,
                                           const FieldMask &user_mask)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return false;
    } 

    //--------------------------------------------------------------------------
    void ReductionView::notify_active(void)
    //--------------------------------------------------------------------------
    {
      manager->add_nested_gc_ref(did);
    }

    //--------------------------------------------------------------------------
    void ReductionView::notify_inactive(void)
    //--------------------------------------------------------------------------
    {
      // No need to check for deletion of the manager since
      // we know that we also hold a resource reference
      manager->remove_nested_gc_ref(did);
    }

    //--------------------------------------------------------------------------
    void ReductionView::notify_valid(void)
    //--------------------------------------------------------------------------
    {
      manager->add_nested_valid_ref(did);
    }

    //--------------------------------------------------------------------------
    void ReductionView::notify_invalid(void)
    //--------------------------------------------------------------------------
    {
      manager->remove_nested_valid_ref(did);
    }

    //--------------------------------------------------------------------------
    void ReductionView::collect_users(const std::set<Event> &term_events)
    //--------------------------------------------------------------------------
    {
      // Do not do this if we are in LegionSpy so we can see 
      // all of the dependences
#if !defined(LEGION_SPY) && !defined(EVENT_GRAPH_TRACE)
      AutoLock v_lock(view_lock);
      for (std::set<Event>::const_iterator it = term_events.begin();
            it != term_events.end(); it++)
      {
        filter_local_users(*it); 
      }
#endif
    }

    //--------------------------------------------------------------------------
    void ReductionView::send_view(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(is_owner());
      assert(logical_node->is_region()); // Always regions at the top
#endif
      // Don't take the lock, it's alright to have duplicate sends
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(did);
        rez.serialize(manager->did);
        rez.serialize(logical_node->as_region_node()->handle);
        rez.serialize(owner_space);
        rez.serialize<UniqueID>(owner_context->get_context_id());
      }
      runtime->send_reduction_view(target, rez);
      update_remote_instances(target);
    }

    //--------------------------------------------------------------------------
    void ReductionView::send_view_updates(AddressSpaceID target,
                                          const FieldMask &update_mask)
    //--------------------------------------------------------------------------
    {
      Serializer reduction_rez, reading_rez;
      std::deque<PhysicalUser*> red_users, read_users;
      unsigned reduction_events = 0, reading_events = 0;
      {
        AutoLock v_lock(view_lock,1,false/*exclusive*/);
        for (LegionMap<Event,EventUsers>::aligned::const_iterator rit = 
              reduction_users.begin(); rit != reduction_users.end(); rit++)
        {
          FieldMask overlap = rit->second.user_mask & update_mask;
          if (!overlap)
            continue;
          reduction_events++;
          const EventUsers &event_users = rit->second;
          reduction_rez.serialize(rit->first);
          if (event_users.single)
          {
            reduction_rez.serialize<size_t>(1);
            reduction_rez.serialize(overlap);
            red_users.push_back(event_users.users.single_user);
          }
          else
          {
            reduction_rez.serialize<size_t>(
                                      event_users.users.multi_users->size());
            // Just send them all
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator
                  it = event_users.users.multi_users->begin(); it != 
                  event_users.users.multi_users->end(); it++)
            {
              reduction_rez.serialize(it->second);
              red_users.push_back(it->first);
            }
          }
        }
        for (LegionMap<Event,EventUsers>::aligned::const_iterator rit = 
              reading_users.begin(); rit != reading_users.end(); rit++)
        {
          FieldMask overlap = rit->second.user_mask & update_mask;
          if (!overlap)
            continue;
          reading_events++;
          const EventUsers &event_users = rit->second;
          reading_rez.serialize(rit->first);
          if (event_users.single)
          {
            reading_rez.serialize<size_t>(1);
            reading_rez.serialize(overlap);
            read_users.push_back(event_users.users.single_user);
          }
          else
          {
            reading_rez.serialize<size_t>(
                                      event_users.users.multi_users->size());
            // Just send them all
            for (LegionMap<PhysicalUser*,FieldMask>::aligned::const_iterator
                  it = event_users.users.multi_users->begin(); it != 
                  event_users.users.multi_users->end(); it++)
            {
              reading_rez.serialize(it->second);
              read_users.push_back(it->first);
            }
          }
        }
      }
      // We've released the lock, so reassemble the message
      Serializer rez;
      {
        RezCheck z(rez);
#ifdef DEBUG_HIGH_LEVEL
        assert(logical_node->is_region());
#endif
        rez.serialize(logical_node->as_region_node()->handle);
        rez.serialize(did);
        rez.serialize<size_t>(red_users.size());
        for (std::deque<PhysicalUser*>::const_iterator it = 
              red_users.begin(); it != red_users.end(); it++)
        {
          (*it)->pack_user(rez);
        }
        rez.serialize<size_t>(read_users.size());
        for (std::deque<PhysicalUser*>::const_iterator it = 
              read_users.begin(); it != read_users.end(); it++)
        {
          (*it)->pack_user(rez);
        }
        rez.serialize(reduction_events);
        size_t reduction_size = reduction_rez.get_used_bytes(); 
        rez.serialize(reduction_rez.get_buffer(), reduction_size);
        rez.serialize(reading_events);
        size_t reading_size = reading_rez.get_used_bytes();
        rez.serialize(reading_rez.get_buffer(), reading_size);
      }
      runtime->send_reduction_update(target, rez);
    }

    //--------------------------------------------------------------------------
    void ReductionView::process_update(Deserializer &derez, 
                                       AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      size_t num_reduction_users;
      derez.deserialize(num_reduction_users);
      std::vector<PhysicalUser*> red_users(num_reduction_users);
      FieldSpaceNode *field_node = logical_node->column_source;
      for (unsigned idx = 0; idx < num_reduction_users; idx++)
        red_users[idx] = PhysicalUser::unpack_user(derez, field_node, 
                                                   source, true/*add ref*/);
      size_t num_reading_users;
      derez.deserialize(num_reading_users);
      std::deque<PhysicalUser*> read_users(num_reading_users);
      for (unsigned idx = 0; idx < num_reading_users; idx++)
        read_users[idx] = PhysicalUser::unpack_user(derez, field_node, 
                                                    source, true/*add ref*/);
      std::deque<Event> collect_events;
      {
        unsigned reduction_index = 0, reading_index = 0;
        unsigned num_reduction_events;
        derez.deserialize(num_reduction_events);
        AutoLock v_lock(view_lock);
        for (unsigned idx = 0; idx < num_reduction_events; idx++)
        {
          Event red_event;
          derez.deserialize(red_event);
          size_t num_users;
          derez.deserialize(num_users);
          for (unsigned idx2 = 0; idx2 < num_users; idx2++)
          {
            FieldMask user_mask;
            derez.deserialize(user_mask);
            add_physical_user(red_users[reduction_index++], false/*reading*/,
                              red_event, user_mask);
          }
          if (outstanding_gc_events.find(red_event) == 
              outstanding_gc_events.end())
          {
            outstanding_gc_events.insert(red_event);
            collect_events.push_back(red_event);
          }
        }
        unsigned num_reading_events;
        derez.deserialize(num_reading_events);
        for (unsigned idx = 0; idx < num_reading_events; idx++)
        {
          Event read_event;
          derez.deserialize(read_event);
          size_t num_users;
          derez.deserialize(num_users);
          for (unsigned idx2 = 0; idx2 < num_users; idx2++)
          {
            FieldMask user_mask;
            derez.deserialize(user_mask);
            add_physical_user(read_users[reading_index++], true/*reading*/,
                              read_event, user_mask);
          }
          if (outstanding_gc_events.find(read_event) ==
              outstanding_gc_events.end())
          {
            outstanding_gc_events.insert(read_event);
            collect_events.push_back(read_event);
          }
        }
      }
      if (!collect_events.empty())
      {
        for (std::deque<Event>::const_iterator it = collect_events.begin();
              it != collect_events.end(); it++)
        {
          defer_collect_user(*it);
        }
      }
    }

    //--------------------------------------------------------------------------
    Memory ReductionView::get_location(void) const
    //--------------------------------------------------------------------------
    {
      return manager->get_memory();
    }

    //--------------------------------------------------------------------------
    ReductionOpID ReductionView::get_redop(void) const
    //--------------------------------------------------------------------------
    {
      return manager->redop;
    }

    //--------------------------------------------------------------------------
    /*static*/ void ReductionView::handle_send_reduction_view(Runtime *runtime,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez); 
      DistributedID did;
      derez.deserialize(did);
      DistributedID manager_did;
      derez.deserialize(manager_did);
      LogicalRegion handle;
      derez.deserialize(handle);
      AddressSpaceID owner_space;
      derez.deserialize(owner_space);
      UniqueID context_uid;
      derez.deserialize(context_uid);

      RegionNode *target_node = runtime->forest->get_node(handle);
      Event man_ready = Event::NO_EVENT;
      PhysicalManager *phy_man = 
        runtime->find_or_request_physical_manager(manager_did, man_ready);
      if (man_ready.exists())
        man_ready.wait();
#ifdef DEBUG_HIGH_LEVEL
      assert(phy_man->is_reduction_manager());
#endif
      ReductionManager *red_manager = phy_man->as_reduction_manager();
      SingleTask *owner_context = runtime->find_context(context_uid);
      void *location;
      if (runtime->find_pending_collectable_location(did, location))
        legion_new_in_place<ReductionView>(location, runtime->forest,
                                           did, owner_space,
                                           runtime->address_space,
                                           target_node, red_manager,
                                           owner_context);
      else
        legion_new<ReductionView>(runtime->forest, did, owner_space,
                                  runtime->address_space, target_node,
                                  red_manager, owner_context);
    }

    //--------------------------------------------------------------------------
    /*static*/ void ReductionView::handle_send_update(Runtime *runtime,
                                     Deserializer &derez, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      LogicalRegion handle;
      derez.deserialize(handle);
      DistributedID did;
      derez.deserialize(did);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_HIGH_LEVEL
      ReductionView *red_view = dynamic_cast<ReductionView*>(dc);
      assert(red_view != NULL);
#else
      ReductionView *red_view = static_cast<ReductionView*>(dc);
#endif
      red_view->process_update(derez, source);
    }

  }; // namespace Internal 
}; // namespace Legion

