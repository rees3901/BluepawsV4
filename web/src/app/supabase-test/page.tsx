import { connection } from 'next/server'

import { createSupabaseClient } from '@/lib/supabase'

export default async function SupabaseTestPage() {
  await connection()

  let data: unknown = null
  let errorMessage: string | null = null

  try {
    const supabase = createSupabaseClient()
    const result = await supabase
      .from('positions')
      .select(
        'id, device_uid, message_id, latitude, longitude, battery, source, recorded_at'
      )
      .order('recorded_at', { ascending: false })
      .limit(1)

    if (result.error) {
      throw result.error
    }

    data = result.data
  } catch (error) {
    errorMessage = error instanceof Error ? error.message : 'Unknown Supabase error'
  }

  return (
    <main style={{ padding: '40px' }}>
      <h1>Supabase Test</h1>
      <pre>
        {errorMessage
          ? `Unable to read positions: ${errorMessage}`
          : JSON.stringify(data, null, 2)}
      </pre>
    </main>
  )
}
