/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_device.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "genxml/gen_macros.h"
#include "util/macros.h"

#include "decode.h"

#include "panvk_private.h"
#include "panvk_cs.h"

#include "vk_drm_syncobj.h"

static int
panvk_submit_kbase_batch(struct panfrost_device *pdev,
                         mali_ptr jc,
                         uint32_t requirements,
                         struct kbase_syncobj *syncobj,
                         uint32_t *bos,
                         unsigned nr_bos)
{

   pdev->mali.handle_events(&pdev->mali);

   int atom = pdev->mali.submit(&pdev->mali,
                        jc,
                        requirements,
                        syncobj,
                        (int32_t *)bos,
                        nr_bos);

   if (atom < 0) {
      errno = EINVAL;
      return -1;
   }

   return 0;
}

static void
panvk_queue_submit_batch(struct panvk_queue *queue,
                         struct panvk_batch *batch,
                         uint32_t *bos, unsigned nr_bos,
                         uint32_t *in_fences,
                         unsigned nr_in_fences)
{
   const struct panvk_device *dev = queue->device;
   unsigned debug = dev->physical_device->instance->debug_flags;
   struct panfrost_device *pdev =
      (struct panfrost_device *)&dev->physical_device->pdev;


   int ret;

   if (batch->issued) {
      util_dynarray_foreach(&batch->jobs, void *, job)
         memset((*job), 0, 4 * 4);

      if (batch->tiler.descs.cpu) {
         memcpy(batch->tiler.descs.cpu,
                batch->tiler.templ,
                pan_size(TILER_CONTEXT) +
                pan_size(TILER_HEAP));
      }
   }

   if (batch->scoreboard.first_job) {

      ret = panvk_submit_kbase_batch(pdev,
                                     batch->scoreboard.first_job,
                                     0,
                                     queue->syncobj_kbase,
                                     bos,
                                     nr_bos);

      assert(!ret);

      if (debug & (PANVK_DEBUG_TRACE | PANVK_DEBUG_SYNC)) {
         pdev->mali.syncobj_wait(&pdev->mali, queue->syncobj_kbase);
      }

      if (debug & PANVK_DEBUG_TRACE)
         GENX(pandecode_jc)(batch->scoreboard.first_job,
                            pdev->gpu_id);

      if (debug & PANVK_DEBUG_DUMP)
         pandecode_dump_mappings();
   }

   if (batch->fragment_job) {

      ret = panvk_submit_kbase_batch(pdev,
                                     batch->fragment_job,
                                     PANFROST_JD_REQ_FS,
                                     queue->syncobj_kbase,
                                     bos,
                                     nr_bos);

      assert(!ret);

      if (debug & (PANVK_DEBUG_TRACE | PANVK_DEBUG_SYNC)) {
         pdev->mali.syncobj_wait(&pdev->mali, queue->syncobj_kbase);
      }

      if (debug & PANVK_DEBUG_TRACE)
         GENX(pandecode_jc)(batch->fragment_job,
                            pdev->gpu_id);

      if (debug & PANVK_DEBUG_DUMP)
         pandecode_dump_mappings();
   }

   if (debug & PANVK_DEBUG_TRACE)
      pandecode_next_frame();

   batch->issued = true;
}

static void
panvk_queue_transfer_sync(struct panvk_queue *queue,
                          struct kbase_syncobj **dst)
{
   struct panfrost_device *pdev =
      (struct panfrost_device *)&queue->device->physical_device->pdev;

   *dst = pdev->mali.syncobj_dup(&pdev->mali,
                                 queue->syncobj_kbase);
}

static void
panvk_add_wait_event_syncobjs(struct panvk_batch *batch,
                              struct kbase_syncobj **waits,
                              unsigned *nr_waits)
{
   util_dynarray_foreach(&batch->event_ops,
                         struct panvk_event_op,
                         op) {

      if (op->type == PANVK_EVENT_OP_WAIT) {
         waits[(*nr_waits)++] = op->event->syncobj_kbase;
      }
   }
}

static void
panvk_signal_event_syncobjs(struct panvk_queue *queue,
                            struct panvk_batch *batch)
{
   struct panfrost_device *pdev =
      (struct panfrost_device *)&queue->device->physical_device->pdev;

   util_dynarray_foreach(&batch->event_ops,
                         struct panvk_event_op,
                         op) {

      switch (op->type) {

      case PANVK_EVENT_OP_SET:
         /* evento recebe cópia do sync da queue */
         op->event->syncobj_kbase =
            pdev->mali.syncobj_dup(&pdev->mali,
                                   queue->syncobj_kbase);
         break;

      case PANVK_EVENT_OP_RESET:
         /* destrói o sync antigo */
         if (op->event->syncobj_kbase)
            pdev->mali.syncobj_destroy(&pdev->mali,
                                       op->event->syncobj_kbase);

         /* cria um novo sync limpo */
         op->event->syncobj_kbase =
            pdev->mali.syncobj_create(&pdev->mali);
         break;

      default:
         break;
      }
   }
}

VkResult
panvk_per_arch(queue_submit)(struct vk_queue *vk_queue,
                             struct vk_queue_submit *submit)
{
   struct panvk_queue *queue =
      container_of(vk_queue, struct panvk_queue, vk);

   struct panvk_device *dev = queue->device;
   struct panfrost_device *pdev =
      (struct panfrost_device *)&dev->physical_device->pdev;

   unsigned nr_semaphores = submit->wait_count;

  for (unsigned i = 0; i < submit->wait_count; i++) {
    struct kbase_syncobj *syncobj = queue->syncobj_kbase;
    pdev->mali.syncobj_wait(&pdev->mali, syncobj);
}

   for (uint32_t j = 0;
        j < submit->command_buffer_count;
        ++j) {

      struct panvk_cmd_buffer *cmdbuf =
         container_of(submit->command_buffers[j],
                      struct panvk_cmd_buffer,
                      vk);

      list_for_each_entry(struct panvk_batch,
                          batch,
                          &cmdbuf->batches,
                          node) {

         unsigned nr_bos =
            panvk_pool_num_bos(&cmdbuf->desc_pool) +
            panvk_pool_num_bos(&cmdbuf->varying_pool) +
            panvk_pool_num_bos(&cmdbuf->tls_pool) +
            (batch->fb.info ?
               batch->fb.info->attachment_count : 0) +
            (batch->blit.src ? 1 : 0) +
            (batch->blit.dst ? 1 : 0) +
            (batch->scoreboard.first_tiler ? 1 : 0) + 1;

         unsigned bo_idx = 0;
         uint32_t bos[nr_bos];

         panvk_pool_get_bo_handles(&cmdbuf->desc_pool,
                                   &bos[bo_idx]);
         bo_idx += panvk_pool_num_bos(&cmdbuf->desc_pool);

         panvk_pool_get_bo_handles(&cmdbuf->varying_pool,
                                   &bos[bo_idx]);
         bo_idx += panvk_pool_num_bos(&cmdbuf->varying_pool);

         panvk_pool_get_bo_handles(&cmdbuf->tls_pool,
                                   &bos[bo_idx]);
         bo_idx += panvk_pool_num_bos(&cmdbuf->tls_pool);

         if (batch->fb.info) {
            for (unsigned i = 0;
                 i < batch->fb.info->attachment_count;
                 i++) {

               bos[bo_idx++] =
                  batch->fb.info->attachments[i]
                  .iview->pview.image->data.bo->gem_handle;
            }
         }

         if (batch->blit.src)
            bos[bo_idx++] = batch->blit.src->gem_handle;

         if (batch->blit.dst)
            bos[bo_idx++] = batch->blit.dst->gem_handle;

         if (batch->scoreboard.first_tiler)
            bos[bo_idx++] = pdev->tiler_heap->gem_handle;

         bos[bo_idx++] = pdev->sample_positions->gem_handle;

         assert(bo_idx == nr_bos);

         for (unsigned x = 0; x < nr_bos; x++) {
            for (unsigned y = x + 1; y < nr_bos;) {
               if (bos[x] == bos[y])
                  bos[y] = bos[--nr_bos];
               else
                  y++;
            }
         }

         panvk_queue_submit_batch(queue,
                                   batch,
                                   bos,
                                   nr_bos,
                                   NULL,
                                   0);

         panvk_signal_event_syncobjs(queue, batch);
      }
   }

   for (unsigned i = 0; i < submit->signal_count; i++) {
        submit->signals[i].sync;

   pdev->mali.syncobj_dup(&pdev->mali,
                       queue->syncobj_kbase);

}


   return VK_SUCCESS;
}

VkResult
panvk_per_arch(CreateSampler)(VkDevice _device,
                              const VkSamplerCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkSampler *pSampler)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_sampler *sampler;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = vk_object_alloc(&device->vk, pAllocator, sizeof(*sampler),
                             VK_OBJECT_TYPE_SAMPLER);
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   STATIC_ASSERT(sizeof(sampler->desc) >= pan_size(SAMPLER));
   panvk_per_arch(emit_sampler)(pCreateInfo, &sampler->desc);
   *pSampler = panvk_sampler_to_handle(sampler);

   return VK_SUCCESS;
}
